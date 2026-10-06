/*
 * profiler.c — per-CPU lock-free sampling + syscall / scheduler / SDL frame markers.
 */

#include "profiler.h"
#include "atomic.h"
#include "exec.h"
#include "hpet.h"
#include "mem.h"
#include "smp.h"
#include "syscall.h"
#include "thread.h"
#include "cmd.h"
#include "serial.h"
#include "string.h"
#include "kernel_symtab.h"
#include "stdio.h"

#include "../include/SDL.h"
#include "../include/indevos_syscall_numbers.h"

#define PROF_CAP_SHIFT 11u
#define PROF_CAP       (1u << PROF_CAP_SHIFT)
#define PROF_CAP_MASK  ((uint64_t)PROF_CAP - 1u)

#ifndef MAX_CPUS
#define MAX_CPUS 16
#endif

/* Live flame aggregation (folded stacks) + syscall timing. */
#define PROF_WALK_DEPTH   6
#define PROF_FLAME_SLOTS  384
#define PROF_SYM_LEN      40
#define PROF_PATH_LEN     256
#define PROF_SC_MAX       128

static volatile bool prof_running;
static volatile bool prof_overlay_ui;

static uint32_t prof_sample_divisor = 8;

static uint64_t prof_sc_enter_ts[MAX_CPUS];

static prof_rec_t prof_ring[MAX_CPUS][PROF_CAP];
static volatile uint64_t prof_seq[MAX_CPUS];

static volatile uint64_t prof_frames_presented;

static volatile uint64_t ov_agg_sc_enter;
static volatile uint64_t ov_agg_sample;
static volatile uint64_t ov_agg_sched;
static volatile uint64_t ov_agg_frame;

typedef struct {
    char     path[PROF_PATH_LEN];
    uint32_t count;
    uint8_t  used;
} prof_flame_slot_t;

static prof_flame_slot_t flame_slots[PROF_FLAME_SLOTS];
static volatile uint64_t flame_samples;
static volatile uint64_t flame_dropped;

typedef struct {
    uint64_t count;
    uint64_t total_us;
    uint64_t max_us;
} prof_sc_slot_t;

static prof_sc_slot_t sc_slots[PROF_SC_MAX];

static void profiler_agg_reset(void) {
    memset(flame_slots, 0, sizeof flame_slots);
    memset(sc_slots, 0, sizeof sc_slots);
    flame_samples = 0;
    flame_dropped = 0;
}

uint64_t profiler_now_us(void) {
    if (hpet_main_cnt && hpet_freq > 0) {
        return *hpet_main_cnt / (hpet_freq / 1000000ULL);
    }
    return hpet_get_ticks() * 1000ULL;
}

void profiler_init(void) {
    prof_running = false;
    prof_overlay_ui = false;
    prof_frames_presented = 0;
    profiler_reset_buffers();
    profiler_agg_reset();
    memset((void *)prof_sc_enter_ts, 0, sizeof prof_sc_enter_ts);
}

bool profiler_enabled(void) { return prof_running; }

void profiler_set_enabled(bool on) { prof_running = on; }

uint32_t profiler_get_sample_divisor(void) { return prof_sample_divisor; }

void profiler_set_sample_divisor(uint32_t div) {
    if (div < 1) div = 1;
    prof_sample_divisor = div;
}

void profiler_reset_buffers(void) {
    memset((void *)prof_ring, 0, sizeof prof_ring);
    memset((void *)prof_seq, 0, sizeof prof_seq);
    profiler_agg_reset();
}

void profiler_set_overlay(bool on) { prof_overlay_ui = on; }

bool profiler_overlay_enabled(void) { return prof_overlay_ui; }

static inline uint32_t profiler_cpu_slot(void) {
    per_cpu_t *pc = smp_get_current_cpu();
    if (!pc || pc->cpu_id >= MAX_CPUS) return 0;
    return (uint32_t)pc->cpu_id;
}

static inline bool profiler_rbp_is_safe(uint64_t addr) {
    if (addr == 0 || (addr & 7)) return false;
    if (addr < 0x1000ULL) return false;
    /* Non-canonical hole */
    if (addr >= 0x0000800000000000ULL &&
        addr <  0xffff800000000000ULL) return false;
    /*
     * Never walk user stacks from the LAPIC ISR. Doom's RBP chain reaches the
     * unmapped guard at USER_STACK_TOP (fault addr 0x7ffffffff808), which
     * panics the kernel under `prof start 1`. Kernel stacks only.
     */
    if (addr < 0xffff800000000000ULL) return false;
    /* Keep both addr and addr+8 inside the same mapped kernel region. */
    if (addr + 8 < addr) return false;
    return true;
}

static inline void profiler_overlay_accumulate(uint32_t kind) {
    if (!prof_overlay_ui) return;
    switch (kind) {
    case PROF_KIND_SC_ENTER:
        __sync_fetch_and_add(&ov_agg_sc_enter, 1);
        break;
    case PROF_KIND_SAMPLE:
        __sync_fetch_and_add(&ov_agg_sample, 1);
        break;
    case PROF_KIND_SCHED:
        __sync_fetch_and_add(&ov_agg_sched, 1);
        break;
    case PROF_KIND_FRAME:
        __sync_fetch_and_add(&ov_agg_frame, 1);
        break;
    default:
        break;
    }
}

static inline void profiler_push_masked(const prof_rec_t *rec) {
    profiler_overlay_accumulate((uint32_t)rec->kind);
    uint32_t c = rec->cpu;
    if (c >= MAX_CPUS) c = 0;

    uint64_t seq = __atomic_fetch_add(&prof_seq[c], 1, __ATOMIC_RELAXED);
    uint64_t slot = seq & PROF_CAP_MASK;
    prof_ring[c][slot] = *rec;
}

static void profiler_emit(uint8_t kind, uint32_t cpu, uint32_t pid,
                          uint64_t tid, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3) {
    if (!prof_running) return;
    prof_rec_t r;
    memset(&r, 0, sizeof(r));
    r.ts_us = profiler_now_us();
    r.kind = kind;
    r.cpu = (uint8_t)cpu;
    r.pid = pid;
    r.tid = tid;
    r.arg0 = a0;
    r.arg1 = a1;
    r.arg2 = a2;
    r.arg3 = a3;
    profiler_push_masked(&r);
}

static size_t profiler_hex_u64(char *dst, size_t dst_len, uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    if (!dst || dst_len < 3) return 0;
    char tmp[16];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v && n < 16) {
            tmp[n++] = hex[v & 0xf];
            v >>= 4;
        }
    }
    size_t pos = 0;
    dst[pos++] = '0';
    if (pos < dst_len) dst[pos++] = 'x';
    while (n > 0 && pos + 1 < dst_len)
        dst[pos++] = tmp[--n];
    dst[pos] = '\0';
    return pos;
}

static void profiler_sym_copy(char *dst, size_t dst_len, const char *name,
                              uint64_t addr) {
    if (!dst || dst_len == 0) return;
    if (name && name[0] && kstrcmp(name, "unknown") != 0) {
        size_t i = 0;
        for (; i + 1 < dst_len && name[i]; i++) {
            char ch = name[i];
            /* Folded format uses ';' as separator — scrub it. */
            dst[i] = (ch == ';' || ch == ' ' || ch == '\n') ? '_' : ch;
        }
        dst[i] = '\0';
        return;
    }
    /* Fallback: raw address so stacks still merge on the same PC. */
    profiler_hex_u64(dst, dst_len, addr);
}

static uint32_t profiler_hash_path(const char *path) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static void profiler_flame_add(const char *path) {
    if (!path || !path[0]) return;
    uint32_t h = profiler_hash_path(path);
    for (uint32_t probe = 0; probe < PROF_FLAME_SLOTS; probe++) {
        uint32_t i = (h + probe) % PROF_FLAME_SLOTS;
        prof_flame_slot_t *s = &flame_slots[i];
        if (!s->used) {
            size_t n = 0;
            while (path[n] && n + 1 < PROF_PATH_LEN) {
                s->path[n] = path[n];
                n++;
            }
            s->path[n] = '\0';
            s->used = 1;
            s->count = 1;
            return;
        }
        if (kstrcmp(s->path, path) == 0) {
            s->count++;
            return;
        }
    }
    __sync_fetch_and_add(&flame_dropped, 1);
}

static const char *profiler_syscall_name(uint64_t n) {
    switch (n) {
    case SYS_EXIT: return "exit";
    case SYS_READ: return "read";
    case SYS_WRITE: return "write";
    case SYS_OPEN: return "open";
    case SYS_CLOSE: return "close";
    case SYS_BRK: return "brk";
    case SYS_SLEEP: return "sleep";
    case SYS_TIME: return "time";
    case SYS_PRINT: return "print";
    case SYS_SDL_INIT: return "sdl_init";
    case SYS_SDL_CREATE_WINDOW: return "sdl_create_window";
    case SYS_SDL_CREATE_RENDERER: return "sdl_create_renderer";
    case SYS_SDL_RENDER_PRESENT: return "sdl_render_present";
    case SYS_SDL_POLL_EVENT: return "sdl_poll_event";
    case SYS_SDL_GET_TICKS: return "sdl_get_ticks";
    case SYS_SDL_DELAY: return "sdl_delay";
    case SYS_SDL_COPY_PIXELS: return "sdl_copy_pixels";
    case SYS_SDL_GET_KEYBOARD_STATE: return "sdl_get_keyboard_state";
    case SYS_SDL_GET_DISPLAY_SIZE: return "sdl_get_display_size";
    case SYS_DESKTOP_EMBED_INFO: return "desktop_embed_info";
    case SYS_DESKTOP_COMPOSITE: return "desktop_composite";
    case SYS_GETTIMEOFDAY: return "gettimeofday";
    case SYS_POLL: return "poll";
    default: return NULL;
    }
}

void profiler_sample_from_isr(registers_t *r) {
    static volatile uint64_t sample_tick[MAX_CPUS];

    if (!prof_running || !r) return;

    per_cpu_t *pc_cpu = smp_get_current_cpu();
    thread_t *thr_pc = pc_cpu ? pc_cpu->current_thread : NULL;
    /* Exiting/halted threads often still have user RBP in the frame while CS
     * is already ring-0 (sys_exit / exec_spawned_exit). Skip them. */
    if (thr_pc && thr_pc->state == THREAD_TERMINATED)
        return;

    uint32_t c = profiler_cpu_slot();
    uint64_t t = __atomic_fetch_add(&sample_tick[c], 1, __ATOMIC_RELAXED)
                 % (uint64_t)prof_sample_divisor;
    if (t != 0) return;

    uint64_t frames[PROF_WALK_DEPTH];
    int depth = 0;
    frames[depth++] = r->rip;

    int user_mode = (r->cs & 3u) ? 1 : 0;
    process_t *up = exec_user_as_process();

    /* Only walk when RBP is a kernel address. int $0x80 leaves the user RBP
     * in the register file, so ring-0 CS alone is not enough — that is what
     * crashed after Doom's Ctrl+C → sys_exit (fault at 0x7ffffffff808). */
    if (profiler_rbp_is_safe(r->rbp)) {
        uint64_t walk_rbp = r->rbp;
        for (int dep = 0; depth < PROF_WALK_DEPTH &&
                          profiler_rbp_is_safe(walk_rbp) &&
                          profiler_rbp_is_safe(walk_rbp + 8);
             dep++) {
            uint64_t ret = *(uint64_t *)(walk_rbp + 8);
            walk_rbp = *(uint64_t *)walk_rbp;
            if (ret == 0)
                break;
            frames[depth++] = ret;
            if (walk_rbp == 0)
                break;
        }
    }
    (void)user_mode;

    uint64_t tid = thr_pc ? thr_pc->id : 0;
    uint32_t pid = up ? (uint32_t)up->pid : 0;

    /* Build folded path: root;...;leaf (leaf = interrupted RIP). */
    char path[PROF_PATH_LEN];
    size_t pos = 0;
    const char *root = user_mode ? "user" : "kernel";
    for (size_t i = 0; root[i] && pos + 1 < sizeof(path); i++)
        path[pos++] = root[i];
    if (pid && pos + 12 < sizeof(path)) {
        path[pos++] = '[';
        path[pos++] = 'p';
        path[pos++] = 'i';
        path[pos++] = 'd';
        /* decimal pid, up to 5 digits */
        char digs[8];
        int nd = 0;
        uint32_t pv = pid;
        if (pv == 0) digs[nd++] = '0';
        while (pv && nd < 8) {
            digs[nd++] = (char)('0' + (pv % 10));
            pv /= 10;
        }
        while (nd > 0)
            path[pos++] = digs[--nd];
        path[pos++] = ']';
    }

    for (int i = depth - 1; i >= 0; i--) {
        if (pos + 1 >= sizeof(path)) break;
        path[pos++] = ';';
        char sym[PROF_SYM_LEN];
        const char *name = exec_lookup_symbol(up, frames[i]);
        profiler_sym_copy(sym, sizeof(sym), name, frames[i]);
        for (size_t k = 0; sym[k] && pos + 1 < sizeof(path); k++)
            path[pos++] = sym[k];
    }
    path[pos] = '\0';
    profiler_flame_add(path);
    __sync_fetch_and_add(&flame_samples, 1);

    prof_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.ts_us = profiler_now_us();
    rec.kind = PROF_KIND_SAMPLE;
    rec.cpu = (uint8_t)c;
    rec.unused = (uint16_t)(user_mode & 1);
    rec.pid = pid;
    rec.tid = tid;
    /* Ring keeps leaf + first 3 callers for host-side tools. */
    rec.arg0 = frames[0];
    rec.arg1 = depth > 1 ? frames[1] : 0;
    rec.arg2 = depth > 2 ? frames[2] : 0;
    rec.arg3 = depth > 3 ? frames[3] : 0;
    profiler_push_masked(&rec);
}

void profiler_syscall_enter(uint64_t syscall_num) {
    if (!prof_running) return;

    uint32_t c = profiler_cpu_slot();

    process_t *p = get_current_process();
    per_cpu_t *pc = smp_get_current_cpu();
    thread_t *thr = pc ? pc->current_thread : NULL;

    uint32_t pid = p ? (uint32_t)p->pid : 0;
    uint64_t tid = thr ? thr->id : 0;

    prof_sc_enter_ts[c] = profiler_now_us();

    profiler_emit(PROF_KIND_SC_ENTER, c, pid, tid,
                  syscall_num, 0, 0, 0);
}

void profiler_syscall_leave(uint64_t syscall_num, uint64_t retval) {
    if (!prof_running) return;

    uint32_t c = profiler_cpu_slot();

    uint64_t t1 = profiler_now_us();
    uint64_t t0 = prof_sc_enter_ts[c];
    uint64_t dur_us = (t1 > t0) ? (t1 - t0) : 1;

    process_t *p = get_current_process();
    per_cpu_t *pc = smp_get_current_cpu();
    thread_t *thr = pc ? pc->current_thread : NULL;
    uint32_t pid = p ? (uint32_t)p->pid : 0;
    uint64_t tid = thr ? thr->id : 0;

    profiler_emit(PROF_KIND_SC_EXIT, c, pid, tid,
                  syscall_num, dur_us, retval, 0);

    if (syscall_num < PROF_SC_MAX) {
        prof_sc_slot_t *s = &sc_slots[syscall_num];
        s->count++;
        s->total_us += dur_us;
        if (dur_us > s->max_us)
            s->max_us = dur_us;
    }
}

void profiler_sched_switch(thread_t *from, thread_t *to) {
    if (!prof_running) return;

    uint32_t c = profiler_cpu_slot();

    uint64_t f_id = from ? from->id : 0;
    uint64_t t_id = to ? to->id : 0;

    uint32_t f_pid = from && from->owner_process
                         ? ((process_t *)from->owner_process)->pid
                         : 0;

    uint32_t t_pid = to && to->owner_process ? ((process_t *)to->owner_process)->pid
                                             : 0;

    profiler_emit(PROF_KIND_SCHED, c, f_pid, f_id, t_id,
                  (uint64_t)t_pid, 0, 0);
}

void profiler_on_frame_present(uint64_t window_ptr_u64) {
    if (!prof_running) return;
    uint32_t c = profiler_cpu_slot();
    uint64_t n = __sync_add_and_fetch(&prof_frames_presented, 1);
    profiler_emit(PROF_KIND_FRAME, c, 0, n,
                  window_ptr_u64, 0, 0, 0);
}

void profiler_print_stats(void) {
    printf("profiler: running=%d sample_div=%u overlay=%d frames=%llu\n",
           prof_running ? 1 : 0,
           (unsigned)profiler_get_sample_divisor(),
           prof_overlay_ui ? 1 : 0,
           (unsigned long long)prof_frames_presented);
    printf("  flame samples=%llu dropped=%llu\n",
           (unsigned long long)flame_samples,
           (unsigned long long)flame_dropped);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        uint64_t seq = prof_seq[i];
        if (seq == 0) continue;
        printf("  cpu%u seq=%llu (ring %u slots, lossy)\n",
               (unsigned)i, (unsigned long long)seq,
               (unsigned)PROF_CAP);
    }
}

void profiler_dump_flame(void) {
    bool was = prof_running;
    prof_running = false;

    /* Collect used slots, sort by count descending (simple selection). */
    uint32_t idx[PROF_FLAME_SLOTS];
    uint32_t n = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < PROF_FLAME_SLOTS; i++) {
        if (!flame_slots[i].used || flame_slots[i].count == 0)
            continue;
        idx[n++] = i;
        total += flame_slots[i].count;
    }
    for (uint32_t a = 0; a < n; a++) {
        uint32_t best = a;
        for (uint32_t b = a + 1; b < n; b++) {
            if (flame_slots[idx[b]].count > flame_slots[idx[best]].count)
                best = b;
        }
        uint32_t tmp = idx[a];
        idx[a] = idx[best];
        idx[best] = tmp;
    }

    if (total == 0) {
        printf("profiler flame: no samples (prof start, run doom, then prof flame)\n");
        prof_running = was;
        return;
    }

    printf("--- PROFILER FLAME (%llu samples, %u unique stacks) ---\n",
           (unsigned long long)total, (unsigned)n);
    serial_puts("--- PROFILER FOLDED (speedscope / flamegraph.pl) ---\n");

    uint32_t show = n < 40 ? n : 40;
    for (uint32_t i = 0; i < show; i++) {
        const prof_flame_slot_t *s = &flame_slots[idx[i]];
        unsigned pct = (unsigned)((s->count * 100ull) / total);
        printf("%3u%% %6u  %s\n", pct, (unsigned)s->count, s->path);
    }
    /* Full folded dump to serial for host conversion. */
    for (uint32_t i = 0; i < n; i++) {
        const prof_flame_slot_t *s = &flame_slots[idx[i]];
        serial_printf("%s %u\n", s->path, (unsigned)s->count);
    }
    serial_puts("--- END FOLDED ---\n");
    printf("(full folded stacks also on serial; pipe through tools/kp_to_flame.py)\n");
    prof_running = was;
}

void profiler_dump_syscalls(void) {
    bool was = prof_running;
    prof_running = false;

    uint32_t idx[PROF_SC_MAX];
    uint32_t n = 0;
    uint64_t total_us = 0;
    for (uint32_t i = 0; i < PROF_SC_MAX; i++) {
        if (sc_slots[i].count == 0) continue;
        idx[n++] = i;
        total_us += sc_slots[i].total_us;
    }
    for (uint32_t a = 0; a < n; a++) {
        uint32_t best = a;
        for (uint32_t b = a + 1; b < n; b++) {
            if (sc_slots[idx[b]].total_us > sc_slots[idx[best]].total_us)
                best = b;
        }
        uint32_t tmp = idx[a];
        idx[a] = idx[best];
        idx[best] = tmp;
    }

    if (n == 0) {
        printf("profiler syscalls: no SC_EXIT records yet\n");
        prof_running = was;
        return;
    }

    printf("--- PROFILER SYSCALLS (by total time, %llu us) ---\n",
           (unsigned long long)total_us);
    printf("%-22s %8s %12s %10s %8s\n",
           "syscall", "count", "total_us", "avg_us", "max_us");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t num = idx[i];
        const prof_sc_slot_t *s = &sc_slots[num];
        const char *name = profiler_syscall_name(num);
        char fallback[16];
        if (!name) {
            snprintf(fallback, sizeof(fallback), "sys_%u", (unsigned)num);
            name = fallback;
        }
        uint64_t avg = s->count ? (s->total_us / s->count) : 0;
        unsigned pct = total_us ? (unsigned)((s->total_us * 100ull) / total_us) : 0;
        printf("%3u%% %-18s %8llu %12llu %10llu %8llu\n",
               pct, name,
               (unsigned long long)s->count,
               (unsigned long long)s->total_us,
               (unsigned long long)avg,
               (unsigned long long)s->max_us);
    }
    prof_running = was;
}

void profiler_dump_ascii(void) {
    serial_puts("--- PROFILER DUMP (prefix KP,v1,) ---\n");
    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        uint64_t max_n = prof_seq[cpu];
        if (max_n == 0) continue;
        uint64_t n = max_n;
        if (n > PROF_CAP) n = PROF_CAP;
        uint64_t start_seq = max_n > n ? (max_n - n) : 0;
        for (uint64_t k = 0; k < n; k++) {
            uint64_t seq = start_seq + k;
            uint64_t slot = seq & PROF_CAP_MASK;
            const prof_rec_t *r = &prof_ring[cpu][slot];
            if (r->kind == 0) continue;
            serial_printf(
                "KP,v1,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned)r->kind, (unsigned)r->unused,
                (unsigned)r->cpu, (unsigned)r->pid,
                (unsigned long long)r->tid, (unsigned long long)r->ts_us,
                (unsigned long long)r->arg0, (unsigned long long)r->arg1,
                (unsigned long long)r->arg2, (unsigned long long)r->arg3);
        }
    }
    serial_puts("--- END PROFILER DUMP ---\n");
}

void profiler_on_sdl_present(struct SDL_Window *window) {
    if (!prof_overlay_ui || !window || !window->surface) return;

    uint64_t sc = ov_agg_sc_enter;
    uint64_t sa = ov_agg_sample;
    uint64_t sch = ov_agg_sched;
    uint64_t fr = ov_agg_frame;
    __atomic_store_n(&ov_agg_sc_enter, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ov_agg_sample, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ov_agg_sched, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ov_agg_frame, 0, __ATOMIC_RELAXED);

    SDL_Surface *s = window->surface;
    int w = s->w;
    int bar_h = 24;
    if (bar_h > s->h / 4) bar_h = s->h / 4;
    if (bar_h < 8) bar_h = 8;

    uint32_t bg = SDL_MapRGBA(s->format, 20, 20, 30, 255);
    SDL_Rect full = {0, 0, w, bar_h};
    SDL_FillRect(s, &full, bg);

    uint64_t tot = sc + sa + sch + fr;
    if (tot == 0) tot = 1;

    int x = 0;
    struct {
        uint64_t v;
        uint8_t r, g, b;
    } parts[] = {
        {sc, 80, 200, 120},
        {sa, 220, 160, 60},
        {sch, 200, 80, 200},
        {fr, 100, 140, 255},
    };

    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        int seg = (int)((parts[i].v * (uint64_t)w) / tot);
        if (seg < 1 && parts[i].v > 0) seg = 1;
        SDL_Rect seg_r = {x, 0, seg, bar_h};
        uint32_t col = SDL_MapRGBA(s->format, parts[i].r, parts[i].g,
                                   parts[i].b, 255);
        SDL_FillRect(s, &seg_r, col);
        x += seg;
    }
}

void profiler_cli(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: prof start [div]|stop|reset|stats|dump|flame|syscalls|overlay on|off|sample <n>\n");
        printf("  flame    - folded stack samples (text flamegraph)\n");
        printf("  syscalls - time spent inside each syscall\n");
        return;
    }
    if (kstrcmp(argv[1], "start") == 0) {
        if (argc >= 3) {
            uint32_t d = (uint32_t)atoi(argv[2]);
            profiler_set_sample_divisor(d ? d : 1);
        }
        profiler_set_enabled(true);
        printf("profiler: started (sample divisor=%u)\n",
               (unsigned)profiler_get_sample_divisor());
        return;
    }
    if (kstrcmp(argv[1], "stop") == 0) {
        profiler_set_enabled(false);
        printf("profiler: stopped\n");
        return;
    }
    if (kstrcmp(argv[1], "reset") == 0) {
        profiler_reset_buffers();
        prof_frames_presented = 0;
        printf("profiler: buffers reset\n");
        return;
    }
    if (kstrcmp(argv[1], "stats") == 0) {
        profiler_print_stats();
        return;
    }
    if (kstrcmp(argv[1], "dump") == 0) {
        profiler_dump_ascii();
        return;
    }
    if (kstrcmp(argv[1], "flame") == 0) {
        profiler_dump_flame();
        return;
    }
    if (kstrcmp(argv[1], "syscalls") == 0 || kstrcmp(argv[1], "sc") == 0) {
        profiler_dump_syscalls();
        return;
    }
    if (kstrcmp(argv[1], "overlay") == 0) {
        if (argc < 3) {
            printf("prof overlay on|off\n");
            return;
        }
        if (kstrcmp(argv[2], "on") == 0) profiler_set_overlay(true);
        else if (kstrcmp(argv[2], "off") == 0) profiler_set_overlay(false);
        else {
            printf("prof overlay on|off\n");
            return;
        }
        printf("profiler overlay: %s\n", profiler_overlay_enabled() ? "on" : "off");
        return;
    }
    if (kstrcmp(argv[1], "sample") == 0) {
        if (argc < 3) {
            printf("profiler sample divisor: %u\n",
                   (unsigned)profiler_get_sample_divisor());
            return;
        }
        uint32_t d = (uint32_t)atoi(argv[2]);
        profiler_set_sample_divisor(d ? d : 1);
        printf("profiler sample divisor set to %u\n",
               (unsigned)profiler_get_sample_divisor());
        return;
    }
    printf("Unknown prof command. Use: prof start|stop|flame|syscalls|...\n");
}
