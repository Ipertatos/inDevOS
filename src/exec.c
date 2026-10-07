#include "exec.h"
#include "fat32.h"
#include "vfs.h"
#include "mem.h"
#include "utils.h"
#include "string.h"
#include "serial.h"
#include "debug.h"
#include "gdt.h"
#include "term.h"
#include "tty.h"
#include "lua_runtime.h"
#include "syscall.h"
#include "../include/indevos_errno.h"
#include "vconsole.h"
#include "users.h"
#include "kernel_symtab.h"
#include "keyboard.h"
#include "thread.h"
#include "desktop.h"
#include "ktime.h"
#include "string.h"

/* Serial-only exec load profiler (toggle with scheddebug / always on). */
static uint64_t exec_dbg_t0;

static void exec_dbg_reset(void) {
    exec_dbg_t0 = ktime_now_us();
    serial_printf("[exec +0us] BEGIN\n");
}

void exec_dbg_mark(const char *phase) {
    if (!ktrace_enabled) return;   /* off by default: see ktrace() in debug.h */
    uint64_t now = ktime_now_us();
    serial_printf("[exec +%lluus] %s\n",
                  (unsigned long long)(now - exec_dbg_t0), phase);
}

/* ---- Context for returning from user mode via sys_exit ---- */
static exec_jmpbuf_t  exec_return_ctx;
static exec_jmpbuf_t  exec_return_ctx_saved;
static bool           exec_return_ctx_nested;

void exec_push_return_ctx(void) {
    exec_return_ctx_saved = exec_return_ctx;
    exec_return_ctx_nested = true;
}

void exec_pop_return_ctx(void) {
    if (exec_return_ctx_nested) {
        exec_return_ctx = exec_return_ctx_saved;
        exec_return_ctx_nested = false;
    }
}
static volatile int   exec_exit_status_val   = 0;
static uint64_t      *exec_kernel_pml4_save  = NULL;
static uint64_t       exec_orig_kstack_top   = 0;
/* Ring-3 process from exec_run_elf; preempt must not confuse syscalls / #PF */
static process_t     *exec_ring3_active_proc = NULL;
/* Kernel thread (normally the shell) that entered Ring 3 via exec_file */
static thread_t      *exec_sync_thread = NULL;
static char *const   *exec_current_argv = NULL;

#define EXEC_MAX_ARGS 16
/* Distance between the end of the brk heap start and the first anonymous
 * mmap.  Virtual only (pages are mapped on demand by brk/mmap). */
#define EXEC_MMAP_GAP 0x40000000ULL   /* 1 GiB */

/* Values for the auxiliary vector of the image being launched; filled by
 * exec_collect_auxinfo() right before exec_build_abi_stack(). */
typedef struct {
    uint64_t phdr;      /* AT_PHDR: user address of the program headers */
    uint64_t phent;     /* AT_PHENT */
    uint64_t phnum;     /* AT_PHNUM */
    uint64_t entry;     /* AT_ENTRY */
} exec_auxinfo_t;
static exec_auxinfo_t exec_cur_aux;

/* Environment handed to every new program (Linux binaries read it through
 * envp/environ; the inDevOS crt0 ignores it). */
static const char *const exec_default_envp[] = {
    "PATH=/bin:/",
    "HOME=/",
    "TERM=linux",
    "SHELL=/bin/sh.exe",
    "USER=root",
    NULL
};

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_HWCAP2   26
#define AT_EXECFN   31

/* Not cryptographic: TSC-seeded splitmix64, good enough for AT_RANDOM
 * (musl/glibc use it for the stack-protector canary and pointer guard). */
static uint64_t exec_rand_state;
static uint64_t exec_random_u64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    exec_rand_state += (((uint64_t)hi << 32) | lo) + 0x9E3779B97F4A7C15ULL;
    uint64_t z = exec_rand_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t exec_cpu_hwcap(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return d;   /* Linux AT_HWCAP on x86 = CPUID.1:EDX */
}

/*
 * Build the System V x86-64 initial process stack on the top user-stack page:
 *
 *   [rsp]          argc
 *   [rsp+8]        argv[0..argc-1], NULL
 *                  envp[0..], NULL
 *                  auxv pairs (AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_ENTRY,
 *                  AT_RANDOM, AT_UID/EUID/GID/EGID, AT_SECURE, AT_HWCAP,
 *                  AT_CLKTCK, AT_PLATFORM, AT_EXECFN, ...), AT_NULL
 *   ...            16 random bytes, "x86_64", argument and environment strings
 *
 * rsp is 16-byte aligned.  The inDevOS crt0 only reads argc/argv, so this
 * layout is backwards compatible with the existing apps; static musl/glibc
 * binaries need envp + auxv (AT_PHDR for TLS, AT_RANDOM for the canary).
 */
static uint64_t exec_build_abi_stack(uint64_t *user_pml4, uint64_t ustack_top,
                                     const char *fallback_name)
{
    uint64_t top_page_phys = vmm_is_mapped_in(user_pml4, ustack_top);
    if (!top_page_phys)
        return ustack_top;

    uint8_t *page = (uint8_t *)phys_to_virt(top_page_phys);
    const char *args[EXEC_MAX_ARGS];
    int argc = 0;

    if (exec_current_argv) {
        for (int i = 0; i < EXEC_MAX_ARGS && exec_current_argv[i]; i++) {
            args[i] = exec_current_argv[i];
            argc++;
        }
    }
    if (argc == 0 && fallback_name) {
        args[0] = fallback_name;
        argc = 1;
    }

    uint32_t str_off = PAGE_SIZE;

    /* AT_RANDOM: 16 bytes */
    str_off -= 16;
    for (int i = 0; i < 2; i++) {
        uint64_t r = exec_random_u64();
        memcpy(page + str_off + i * 8, &r, 8);
    }
    uint64_t random_va = ustack_top + str_off;

    /* AT_PLATFORM */
    static const char plat[] = "x86_64";
    str_off -= sizeof(plat);
    memcpy(page + str_off, plat, sizeof(plat));
    uint64_t platform_va = ustack_top + str_off;

    /* Keep room for the pointer table: argc + argv + envp + auxv (~400 B). */
    const uint32_t reserve = 768;

    uint64_t arg_virt[EXEC_MAX_ARGS];
    int stored_args = 0;
    for (int i = 0; i < argc; i++) {
        size_t nlen = strlen(args[i]) + 1;
        if (nlen > 127) nlen = 127;
        if (str_off < nlen + reserve)
            break;
        str_off -= (uint32_t)nlen;
        memcpy(page + str_off, args[i], nlen);
        page[str_off + nlen - 1] = '\0';
        arg_virt[i] = ustack_top + str_off;
        stored_args++;
    }
    argc = stored_args;

    uint64_t env_virt[8];
    int envc = 0;
    for (int i = 0; exec_default_envp[i] && envc < 8; i++) {
        size_t nlen = strlen(exec_default_envp[i]) + 1;
        if (str_off < nlen + reserve)
            break;
        str_off -= (uint32_t)nlen;
        memcpy(page + str_off, exec_default_envp[i], nlen);
        env_virt[envc++] = ustack_top + str_off;
    }

    uint64_t uid = user_current_uid(), gid = user_current_gid();
    uint64_t auxv[][2] = {
        { AT_PHDR,     exec_cur_aux.phdr },
        { AT_PHENT,    exec_cur_aux.phent },
        { AT_PHNUM,    exec_cur_aux.phnum },
        { AT_PAGESZ,   PAGE_SIZE },
        { AT_BASE,     0 },                 /* no dynamic loader */
        { AT_FLAGS,    0 },
        { AT_ENTRY,    exec_cur_aux.entry },
        { AT_UID,      uid },
        { AT_EUID,     uid },
        { AT_GID,      gid },
        { AT_EGID,     gid },
        { AT_SECURE,   0 },
        { AT_HWCAP,    exec_cpu_hwcap() },
        { AT_HWCAP2,   0 },
        { AT_CLKTCK,   100 },
        { AT_RANDOM,   random_va },
        { AT_PLATFORM, platform_va },
        { AT_EXECFN,   argc ? arg_virt[0] : 0 },
        { AT_NULL,     0 },
    };
    uint32_t naux = (uint32_t)(sizeof(auxv) / sizeof(auxv[0]));

    uint32_t nwords = (uint32_t)(1 + argc + 1 + envc + 1 + 2 * naux);
    uint32_t frame_bytes = nwords * 8;
    uint32_t frame_off = (str_off - frame_bytes) & ~(uint32_t)0xF;
    uint64_t *fp = (uint64_t *)(page + frame_off);
    uint32_t w = 0;
    fp[w++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        fp[w++] = arg_virt[i];
    fp[w++] = 0;
    for (int i = 0; i < envc; i++)
        fp[w++] = env_virt[i];
    fp[w++] = 0;
    for (uint32_t i = 0; i < naux; i++) {
        fp[w++] = auxv[i][0];
        fp[w++] = auxv[i][1];
    }

    ktrace("exec: ABI stack @ 0x%llx  argc=%d envc=%d argv0='%s' phdr=0x%llx\n",
                  (unsigned long long)(ustack_top + frame_off),
                  argc, envc, argc ? args[0] : "",
                  (unsigned long long)exec_cur_aux.phdr);
    return ustack_top + frame_off;
}

/* Set by exec_notify_exit after its swapgs; lets callers like sys_exec
 * know that GS needs to be restored when returning through a syscall. */
bool exec_did_swapgs = false;

void exec_set_ring3_active(process_t *p) {
    exec_ring3_active_proc = p;
}

process_t *exec_user_as_process(void) {
    process_t *p = get_current_process();
    /* vfork child runs with current_process == child while
     * exec_ring3_active_proc may still name the suspended parent.
     * COW / demand-paging MUST use the child's PML4 (CR3), not the parent's. */
    if (p && p->is_usermode && p->pml4 && p->fork_parent)
        return p;
    if (get_program_running() && exec_ring3_active_proc &&
        exec_ring3_active_proc->is_usermode && exec_ring3_active_proc->pml4) {
        return exec_ring3_active_proc;
    }
    return p;
}

bool exec_current_is_sync_program(void) {
    return get_program_running() && exec_ring3_active_proc &&
           exec_sync_thread &&
           scheduler_get_current_thread() == exec_sync_thread;
}

/* Called by sys_exit().  Switches back to kernel PML4, restores the
 * original TSS kernel stack, then longjmps into exec_run_elf so the
 * kernel shell loop continues normally. */
__attribute__((noreturn))
void exec_notify_exit(int status) {
    exec_exit_status_val = status;
    exec_ring3_active_proc = NULL;

    /* No swapgs here.  This used to swapgs unconditionally, assuming the
     * caller came through syscall_entry with GS swapped -- but every app
     * calls exit through INT 0x80, which never swaps, so each exit inverted
     * GS.  syscall_entry (SYSCALL / Linux ABI) now restores the user GS
     * before running any C code, so on every path GS is already in its
     * invariant state (GS_BASE=user, KERNEL_GS_BASE=per-CPU). */
    exec_did_swapgs = false;

    /* Restore kernel address space */
    if (exec_kernel_pml4_save) {
        vmm_switch_address_space(exec_kernel_pml4_save);
    }

    /* Restore original TSS/SYSCALL kernel stack so interrupts stay sane
     * after we abandon the user-process kstack below. */
    if (exec_orig_kstack_top) {
        gdt_set_kernel_stack(exec_orig_kstack_top);
        syscall_set_kernel_stack(exec_orig_kstack_top);
    }

    /* Jump back into exec_run_elf at the exec_setjmp() call site.
     * exec_setjmp will return the (non-zero) status value. */
    exec_longjmp(&exec_return_ctx, status ? status : 1);
}

/* Force-terminate a running process from non-syscall context (timer ISR, etc.).
 * Unlike exec_notify_exit, this does NOT perform swapgs because the caller
 * hasn't done one.  Used by the timer interrupt to kill Ring-3 processes
 * stuck in infinite loops, and to kill kernel-mode flat binaries. */
__attribute__((noreturn))
void exec_force_terminate(int status) {
    exec_exit_status_val = status;
    exec_ring3_active_proc = NULL;

    if (exec_kernel_pml4_save) {
        vmm_switch_address_space(exec_kernel_pml4_save);
    }
    if (exec_orig_kstack_top) {
        gdt_set_kernel_stack(exec_orig_kstack_top);
        syscall_set_kernel_stack(exec_orig_kstack_top);
    }

    exec_longjmp(&exec_return_ctx, status ? status : 1);
}

/* Magic bytes for supported formats */
static const uint8_t ELF_MAGIC[4] = {0x7F, 'E', 'L', 'F'};
static const uint8_t CUSTOM_MAGIC[4] = {'E', 'X', 'E', '!'};

/* Simple custom executable header */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];      // "EXE!"
    uint32_t entry_offset;  // Offset from start of file to entry point
} custom_hdr_t;

/* Forward declarations */
static int exec_run_custom(void *image, uint32_t size, const char *name);
static int exec_run_raw(void *image, uint32_t size, const char *name);
static int exec_run_elf(void *image, uint32_t size, const char *name);

/* Debug function to dump executable memory */
static void debug_dump_executable(void *mem, uint32_t size, uint64_t load_addr) {
    printf("exec: DEBUG DUMP - Load address: 0x%llx, Size: %u bytes\n", 
           (unsigned long long)load_addr, size);
    
    // uint8_t *bytes = (uint8_t *)mem;
    // for (uint32_t i = 0; i < size; i += 16) {
    //     printf("0x%llx: ", (unsigned long long)(load_addr + i));
        
    //     // Print hex bytes
    //     for (uint32_t j = 0; j < 16 && (i + j) < size; j++) {
    //         printf("%02x ", bytes[i + j]);
    //     }
        
    //     // Pad if less than 16 bytes
    //     for (uint32_t j = (size - i < 16) ? size - i : 16; j < 16; j++) {
    //         printf("   ");
    //     }
        
    //     printf(" | ");
        
    //     // Print ASCII representation
    //     for (uint32_t j = 0; j < 16 && (i + j) < size; j++) {
    //         uint8_t c = bytes[i + j];
    //         printf("%c", (c >= 32 && c <= 126) ? c : '.');
    //     }
        
    //     printf("\n");
    // }
    printf("exec: End of dump. Entry point at offset 0x%llx\n", 
           (unsigned long long)load_addr);
}

/* Automatically fix common relocation issues in flat binaries */
static void fix_flat_binary_relocations(void *image, uint32_t size, uint64_t load_addr) {
    uint8_t *bytes = (uint8_t *)image;
    uint64_t base_offset = load_addr;
    
    printf("exec: Scanning for relocation patterns...\n");
    
    // Scan for common x86-64 instruction patterns that need fixing
    for (uint32_t i = 0; i < size - 8; i++) {
        // Look for: mov reg, imm64 where imm64 is a small value (likely needs relocation)
        if (bytes[i] == 0x48 && (bytes[i+1] & 0xF8) == 0xB8) { // mov rax/rbx/rcx/etc, imm64
            uint64_t *imm64 = (uint64_t *)&bytes[i+2];
            if (*imm64 < 0x1000 && *imm64 > 0) { // Small positive value, likely an offset
                printf("exec: Fixed mov instruction at +0x%x: 0x%llx -> 0x%llx\n", 
                       i, (unsigned long long)*imm64, (unsigned long long)(base_offset + *imm64));
                *imm64 = base_offset + *imm64;
            }
        }
        
        // Look for: mov [reg], imm32 where imm32 is small
        if (bytes[i] == 0xC7 && bytes[i+1] == 0x05) { // mov [rip+disp], imm32
            uint32_t *imm32 = (uint32_t *)&bytes[i+6];
            if (*imm32 < 0x1000 && *imm32 > 0) {
                printf("exec: Fixed mov [mem] instruction at +0x%x: 0x%x -> 0x%llx\n", 
                       i, *imm32, (unsigned long long)(base_offset + *imm32));
                *imm32 = (uint32_t)(base_offset + *imm32);
            }
        }
    }
}

int exec_file(const char *filename, char *const argv[]) {
    exec_current_argv = argv;
    exec_dbg_reset();
    exec_dbg_mark(filename);

    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return -1;
    }

    char abs_path[256];
    const char *resolved = filename;
    if (filename[0] != '/') {
        abs_path[0] = '/';
        int i = 1;
        for (int j = 0; filename[j] && i < 255; j++)
            abs_path[i++] = filename[j];
        abs_path[i] = 0;
        resolved = abs_path;
    }

    void *buffer = NULL;
    uint32_t fsize = 0;
    int bytes_read = 0;

    if (vfs_has_root_mount()) {
        int n = vfs_read_file_alloc(resolved, &buffer, 0);
        if (n <= 0 || !buffer) {
            printf("exec: failed to read '%s'\n", resolved);
            return -1;
        }
        fsize = (uint32_t)n;
        bytes_read = n;
        exec_dbg_mark("file read complete");
    } else {
        fat32_file_t *file = fat32_open(resolved, "r");
        if (!file) {
            printf("exec: failed to open '%s'\n", resolved);
            return -1;
        }
        fsize = file->file_size;
        if (fsize == 0) {
            printf("exec: file is empty\n");
            fat32_close(file);
            return -1;
        }
        buffer = kalloc(fsize);
        if (!buffer) {
            printf("exec: out of memory allocating %u bytes\n", fsize);
            fat32_close(file);
            return -1;
        }
        bytes_read = fat32_read(file, buffer, fsize);
        fat32_close(file);
        exec_dbg_mark("file read complete");
    }
    if (bytes_read != (int)fsize) {
        printf("exec: failed to read file, expected %u got %d\n", fsize, bytes_read);
        kfree(buffer);
        return -1;
    }

    /* If file seems to be a Lua script (heuristic: .LUA extension or starts with shebang),
       run via Lua interpreter path and return. */
#ifdef ENABLE_LUA
    const char *dot = NULL;
    for (const char *p = filename; *p; ++p) {
        if (*p == '.') dot = p;
    }
    if (dot && (strEql(dot, ".lua") || strEql(dot, ".LUA"))) {
        int lr = lua_run_script_from_buffer(filename, buffer, fsize);
        kfree(buffer);
        return lr;
    }
    if (fsize >= 2) {
        const uint8_t *b = (const uint8_t *)buffer;
        if (b[0] == '#' && b[1] == '!') {
            /* Simple shebang recognition: if it contains 'lua' in the first line, treat as lua. */
            uint32_t up_to = fsize < 128 ? fsize : 128;
            for (uint32_t i = 0; i < up_to; i++) {
                if (b[i] == '\n' || b[i] == '\r') break;
                if (i + 2 < up_to && (b[i] == 'l' || b[i] == 'L') &&
                    (b[i+1] == 'u' || b[i+1] == 'U') &&
                    (b[i+2] == 'a' || b[i+2] == 'A')) {
                    int lr = lua_run_script_from_buffer(filename, buffer, fsize);
                    kfree(buffer);
                    return lr;
                }
            }
        }
    }
#endif

    /* Detect format via magic */
    uint8_t *magic = (uint8_t *)buffer;
    if (memcmp(magic, ELF_MAGIC, 4) == 0) {
        return exec_run_elf(buffer, fsize, filename);
    }

    if (memcmp(magic, CUSTOM_MAGIC, 4) == 0) {
        return exec_run_custom(buffer, fsize, filename);
    }

    /* Fall back to raw binary execution */
    return exec_run_raw(buffer, fsize, filename);
}

/* Execute custom EXE! format */
static int exec_run_custom(void *image, uint32_t size, const char *name) {
    if (size < sizeof(custom_hdr_t)) {
        printf("exec: custom file too small\n");
        kfree(image);
        return -1;
    }

    custom_hdr_t *hdr = (custom_hdr_t *)image;
    uint32_t entry_off = hdr->entry_offset;
    if (entry_off >= size) {
        printf("exec: invalid entry offset %u for file size %u\n", entry_off, size);
        kfree(image);
        return -1;
    }

    printf("exec: executing custom binary '%s' at offset %u\n", name, entry_off);
    debug_dump_executable(image, size, (uint64_t)image);
    fix_flat_binary_relocations(image, size, (uint64_t)image);
    void (*entry)(void) = (void (*)(void))((uint8_t *)image + entry_off);

    /* Prepare for force-terminate via Ctrl+C */
    exec_kernel_pml4_save = NULL;
    exec_orig_kstack_top  = 0;
    set_program_running(true);
    set_terminate_flag(false);

    int exit_val = exec_setjmp(&exec_return_ctx);
    if (exit_val == 0) {
        entry();
        set_program_running(false);
        printf("exec: program '%s' returned\n", name);
        kfree(image);
        return 0;
    }

    /* Arrived here via Ctrl+C force-terminate */
    __asm__ volatile("sti" ::: "memory");
    set_program_running(false);
    set_terminate_flag(false);
    keyboard_set_raw_mode(false);
    printf("\nexec: program '%s' interrupted (^C)\n", name);
    kfree(image);
    return exit_val;
}

/* Execute raw binary assuming entry at image base */
static int exec_run_raw(void *image, uint32_t size, const char *name) {
    (void)size;
    printf("exec: executing raw binary '%s' (entry at base)\n", name);
    debug_dump_executable(image, size, (uint64_t)image);
    fix_flat_binary_relocations(image, size, (uint64_t)image);
    void (*entry)(void) = (void (*)(void))image;

    exec_kernel_pml4_save = NULL;
    exec_orig_kstack_top  = 0;
    set_program_running(true);
    set_terminate_flag(false);

    int exit_val = exec_setjmp(&exec_return_ctx);
    if (exit_val == 0) {
        entry();
        set_program_running(false);
        printf("exec: program '%s' returned\n", name);
        kfree(image);
        return 0;
    }

    __asm__ volatile("sti" ::: "memory");
    set_program_running(false);
    set_terminate_flag(false);
    keyboard_set_raw_mode(false);
    printf("\nexec: program '%s' interrupted (^C)\n", name);
    kfree(image);
    return exit_val;
}

/* -------------------------------------------------- */
/* Minimal in-memory ELF64 loader                    */
/* -------------------------------------------------- */
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define DT_RELA 7     // Relocation table (Elf64_Rela)
#define DT_RELASZ 8   // Size of relocation table
#define DT_RELAENT 9  // Size of each Rela entry

typedef struct __attribute__((packed)) {
    uint64_t d_tag;
    uint64_t d_val;
} Elf64_Dyn;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;  // Offset to apply relocation
    uint64_t r_info;    // Type and symbol index
    int64_t r_addend;   // Constant addend
} Elf64_Rela;

#define ELF64_R_TYPE(i) ((i) & 0xffffffffL)
#define R_X86_64_RELATIVE 8  // Relative relocation type
#define R_X86_64_64       1  // Direct 64-bit relocation
#define R_X86_64_GLOB_DAT 6  // Global data relocation
#define R_X86_64_JUMP_SLOT 7 // Jump slot relocation

/*
 * Page-safe accessors for a user address space being built.
 *
 * The relocation pass used to translate only the first page of each 8-byte
 * relocation entry and of each 8-byte target, then dereference a raw pointer.
 * When an entry or its target straddled a page boundary that read/wrote the
 * *physically* adjacent page instead of the virtually adjacent one, silently
 * corrupting unrelated data.  With few relocations it usually went unnoticed;
 * an optimised (-O2) build has far more of them and crashed soon after start.
 */
static bool exec_user_read(uint64_t *pml4, uint64_t vaddr, void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        uint64_t va = vaddr + i;
        uint64_t phys = vmm_is_mapped_in(pml4, va & ~0xFFFULL);
        if (!phys)
            return false;
        d[i] = *(uint8_t *)((uint8_t *)phys_to_virt(phys) + (va & 0xFFF));
    }
    return true;
}

static bool exec_user_write_u64(uint64_t *pml4, uint64_t vaddr, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        uint64_t va = vaddr + i;
        uint64_t phys = vmm_is_mapped_in(pml4, va & ~0xFFFULL);
        if (!phys)
            return false;
        *(uint8_t *)((uint8_t *)phys_to_virt(phys) + (va & 0xFFF)) =
            (uint8_t)(value >> (i * 8));
    }
    return true;
}

#define PT_INTERP 3
#define PT_PHDR   6
#define ET_EXEC   2
#define ET_DYN    3
#define EM_X86_64 62

/* Reject images this loader cannot run.  Returns NULL if OK, else a reason. */
static const char *exec_elf_unsupported(const void *image, uint32_t size)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    if (eh->e_ident[4] != 2)                      /* EI_CLASS != ELFCLASS64 */
        return "not a 64-bit ELF";
    if (eh->e_machine != EM_X86_64)
        return "not an x86-64 ELF";
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        return "not an executable (ET_EXEC/ET_DYN)";
    if (eh->e_phentsize < sizeof(Elf64_Phdr) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size)
        return "bad program header table";
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)image +
                                eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_INTERP)
            return "dynamically linked (PT_INTERP); only static binaries are supported";
        if (ph->p_type == PT_LOAD && ph->p_filesz &&
            ph->p_offset + ph->p_filesz > size)
            return "PT_LOAD outside the file";
    }
    return NULL;
}

/* Fill exec_cur_aux (AT_PHDR/PHENT/PHNUM/ENTRY) for the image being loaded.
 * Images are mapped at their own p_vaddr (load bias 0). */
static void exec_collect_auxinfo(const void *image)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    uint64_t phdr = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)image +
                                eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_PHDR) { phdr = ph->p_vaddr; break; }
    }
    if (!phdr) {
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)image +
                                    eh->e_phoff + i * eh->e_phentsize);
            if (ph->p_type == PT_LOAD && eh->e_phoff >= ph->p_offset &&
                eh->e_phoff < ph->p_offset + ph->p_filesz) {
                phdr = ph->p_vaddr + (eh->e_phoff - ph->p_offset);
                break;
            }
        }
    }
    exec_cur_aux.phdr  = phdr;
    exec_cur_aux.phent = eh->e_phentsize;
    exec_cur_aux.phnum = eh->e_phnum;
    exec_cur_aux.entry = eh->e_entry;
}

/* Back one page of a PT_LOAD segment.  Static non-PIE binaries (musl-gcc
 * -static) can place two segments in the same page; the second segment must
 * reuse the frame (keeping the first segment's bytes) instead of replacing it.
 * Shared pages get the union of both segments' permissions.
 * Returns 0 on success, -1 if out of memory / mapping failed. */
static int exec_map_segment_page(uint64_t *pml4, uint64_t va, uint64_t flags)
{
    uint64_t existing = vmm_is_mapped_in(pml4, va);
    if (existing) {
        return vmm_map_page_in(pml4, va, existing,
                               VMM_PRESENT | VMM_USER | VMM_WRITABLE);
    }
    uint64_t frame = vmm_alloc_frame();
    if (!frame)
        return -1;
    memset(phys_to_virt(frame), 0, PAGE_SIZE);
    return vmm_map_page_in(pml4, va, frame, flags);
}

/* Apply DT_RELA relocations for a PIE image loaded at its own p_vaddr. */
static void exec_apply_relocations(uint64_t *pml4, const Elf64_Phdr *dyn_ph)
{
    if (!dyn_ph)
        return;

    uint64_t rela = 0, relasz = 0, relaent = 0;
    for (uint64_t off = 0; off + sizeof(Elf64_Dyn) <= dyn_ph->p_memsz;
         off += sizeof(Elf64_Dyn)) {
        Elf64_Dyn dyn;
        if (!exec_user_read(pml4, dyn_ph->p_vaddr + off, &dyn, sizeof(dyn)))
            break;
        if (dyn.d_tag == 0)
            break;
        if (dyn.d_tag == DT_RELA)    rela    = dyn.d_val;
        if (dyn.d_tag == DT_RELASZ)  relasz  = dyn.d_val;
        if (dyn.d_tag == DT_RELAENT) relaent = dyn.d_val;
    }

    if (!rela || !relasz || relaent < sizeof(Elf64_Rela))
        return;

    uint32_t applied = 0;
    for (uint64_t i = 0; i < relasz / relaent; i++) {
        Elf64_Rela r;
        if (!exec_user_read(pml4, rela + i * relaent, &r, sizeof(r)))
            continue;
        uint32_t rtype = ELF64_R_TYPE(r.r_info);
        /* Load bias is 0: segments are mapped at their requested p_vaddr. */
        if (rtype == R_X86_64_RELATIVE || rtype == R_X86_64_64 ||
            rtype == R_X86_64_GLOB_DAT) {
            if (exec_user_write_u64(pml4, r.r_offset, (uint64_t)r.r_addend))
                applied++;
        }
    }
    serial_printf("exec: applied %u relocations (bias=0x0)\n", applied);
}


/* ELF section / symbol (for user backtraces) */
typedef struct __attribute__((packed)) {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct __attribute__((packed)) {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_DYNSYM 11
#define SHN_UNDEF 0
#define STT_FUNC   2
#define STT_IFUNC  10
#define ELF64_ST_TYPE(i) ((unsigned int)(i) & 0xfu)

static void exec_free_user_symbols(process_t *proc) {
    if (!proc) return;
    if (proc->user_symtab) {
        kfree(proc->user_symtab);
        proc->user_symtab = NULL;
    }
    if (proc->user_sym_string_pool) {
        kfree(proc->user_sym_string_pool);
        proc->user_sym_string_pool = NULL;
    }
    proc->user_symtab_count = 0;
}

static void sort_user_symbols_by_addr(user_symbol_t *syms, int n) {
    for (int i = 1; i < n; i++) {
        user_symbol_t key = syms[i];
        int j = i - 1;
        while (j >= 0 && syms[j].addr > key.addr) {
            syms[j + 1] = syms[j];
            j--;
        }
        syms[j + 1] = key;
    }
}

/**
 * Parse .symtab (preferred) or .dynsym from the in-memory ELF image and build
 * a sorted address->name table (functions only). Stripped ELFs have no .symtab;
 * .dynsym may still list some exported symbols.
 */
/* ELF symbol loading gives user-mode fault backtraces their function names.
 * It is not needed to run a program, and it costs a full symtab parse plus two
 * allocations on every launch, so it is off unless requested with `execsym on`
 * (useful when debugging a crashing user program). */
bool exec_user_symbols_enabled = false;

static void exec_load_elf_symbols(void *image, uint32_t image_sz, process_t *proc) {
    exec_free_user_symbols(proc);

    if (!exec_user_symbols_enabled)
        return;

    Elf64_Ehdr *eh = (Elf64_Ehdr *)image;
    if (eh->e_shoff == 0 || eh->e_shnum == 0 || eh->e_shentsize < sizeof(Elf64_Shdr))
        return;
    if (eh->e_shoff + (uint64_t)eh->e_shnum * eh->e_shentsize > image_sz)
        return;

    Elf64_Shdr *shdrs = (Elf64_Shdr *)((uint8_t *)image + eh->e_shoff);
    Elf64_Shdr *sym_sh = NULL;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            sym_sh = &shdrs[i];
            break;
        }
    }
    if (!sym_sh) {
        for (uint16_t i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_DYNSYM) {
                sym_sh = &shdrs[i];
                break;
            }
        }
    }
    if (!sym_sh || sym_sh->sh_link >= eh->e_shnum)
        return;

    Elf64_Shdr *str_sh = &shdrs[sym_sh->sh_link];
    if (str_sh->sh_type != SHT_STRTAB)
        return;
    if (sym_sh->sh_offset + sym_sh->sh_size > image_sz ||
        str_sh->sh_offset + str_sh->sh_size > image_sz)
        return;

    uint64_t entsz = sym_sh->sh_entsize;
    if (entsz < sizeof(Elf64_Sym))
        entsz = sizeof(Elf64_Sym);
    int sym_count = (int)(sym_sh->sh_size / entsz);
    if (sym_count <= 0)
        return;

    const char *strtab = (const char *)image + str_sh->sh_offset;
    size_t strtab_sz = (size_t)str_sh->sh_size;

    int nfunc = 0;
    size_t pool_need = 1;
    for (int j = 0; j < sym_count; j++) {
        Elf64_Sym *sp = (Elf64_Sym *)((uint8_t *)image + sym_sh->sh_offset + (uint64_t)j * entsz);
        int type = (int)ELF64_ST_TYPE(sp->st_info);
        if (type != STT_FUNC && type != STT_IFUNC)
            continue;
        if (sp->st_shndx == SHN_UNDEF || sp->st_value == 0)
            continue;
        if (sp->st_name >= strtab_sz)
            continue;
        const char *nm = strtab + sp->st_name;
        if (!nm[0])
            continue;
        nfunc++;
        pool_need += strlen(nm) + 1;
    }
    if (nfunc == 0)
        return;

    char *pool = (char *)kalloc(pool_need);
    user_symbol_t *syms = (user_symbol_t *)kalloc((size_t)nfunc * sizeof(user_symbol_t));
    if (!pool || !syms) {
        if (pool) kfree(pool);
        if (syms) kfree(syms);
        return;
    }
    pool[0] = '\0';

    int k = 0;
    char *wp = pool;
    for (int j = 0; j < sym_count; j++) {
        Elf64_Sym *sp = (Elf64_Sym *)((uint8_t *)image + sym_sh->sh_offset + (uint64_t)j * entsz);
        int type = (int)ELF64_ST_TYPE(sp->st_info);
        if (type != STT_FUNC && type != STT_IFUNC)
            continue;
        if (sp->st_shndx == SHN_UNDEF || sp->st_value == 0)
            continue;
        if (sp->st_name >= strtab_sz)
            continue;
        const char *nm = strtab + sp->st_name;
        if (!nm[0])
            continue;
        size_t len = strlen(nm) + 1;
        memcpy(wp, nm, len);
        syms[k].addr = sp->st_value;
        syms[k].name = wp;
        wp += len;
        k++;
    }

    sort_user_symbols_by_addr(syms, k);
    proc->user_symtab = syms;
    proc->user_symtab_count = k;
    proc->user_sym_string_pool = pool;
    ktrace("exec: loaded %d ELF symbol(s) for backtraces (build with -g, avoid strip for full names)\n", k);
}

const char *exec_lookup_symbol(process_t *proc, uint64_t addr) {
    if (addr >= KERNEL_VIRT_BASE)
        return lookup_symbol(addr);
    if (proc && proc->user_symtab && proc->user_symtab_count > 0) {
        const user_symbol_t *syms = proc->user_symtab;
        const char *name = "unknown";
        for (int i = 0; i < proc->user_symtab_count; ++i) {
            if (addr < syms[i].addr)
                break;
            name = syms[i].name;
        }
        return name;
    }
    return "unknown";
}

/* User stack size (64 KB) */
/* 64 KB user stack.
 *
 * Do NOT shrink this without fixing address-space teardown first: with 8 pages
 * (32 KB) the *second* program launch crashed in vmm_free_table() walking a
 * garbage page table (faulting address 0x40c900000000, "not present, read,
 * supervisor"), i.e. something in the teardown/frame-reuse path is fragile and
 * only the allocation pattern kept it hidden.  16 pages is stable over repeated
 * launch sweeps. */
#define USER_STACK_PAGES    256  /* 16 * 4096 = 64 KB */
/* Per-process kernel stack (16 KB) */
#define KSTACK_SIZE         (16 * 1024)

/**
 * @brief Load and execute an ELF64 binary in Ring 3.
 *
 * Steps:
 *  1. Parse ELF headers
 *  2. Create a new user address space
 *  3. For each PT_LOAD segment: allocate frames, map into user space, copy data
 *  4. Handle dynamic relocations if present
 *  5. Allocate and map a user stack
 *  6. Allocate a per-process kernel stack
 *  7. Set up TSS RSP0 and per-CPU kernel RSP
 *  8. Switch to the new address space
 *  9. Jump to Ring 3 via enter_usermode() (does not return)
 */
static int exec_run_elf(void *image, uint32_t size, const char *name) {
    exec_dbg_mark("exec_run_elf start");
    exec_did_swapgs = false;
    if (size < sizeof(Elf64_Ehdr)) {
        printf("exec: ELF file too small\n");
        kfree(image);
        return -1;
    }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)image;
    if (eh->e_phoff == 0 || eh->e_phnum == 0) {
        printf("exec: ELF has no program headers\n");
        kfree(image);
        return -1;
    }
    {
        const char *why = exec_elf_unsupported(image, size);
        if (why) {
            printf("exec: %s: %s\n", name, why);
            kfree(image);
            return -1;
        }
    }

    /* ---- Step 1: Find range of loadable segments ---- */
    uint64_t seg_base = UINT64_MAX;
    uint64_t seg_end  = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_filesz == 0 && ph->p_memsz == 0) continue;
        if (ph->p_vaddr < seg_base) seg_base = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > seg_end) seg_end = ph->p_vaddr + ph->p_memsz;
    }

    if (seg_base == UINT64_MAX) {
        printf("exec: no loadable segments\n");
        kfree(image);
        return -1;
    }
    exec_dbg_mark("segments scanned");

    /* ---- Step 2: Create user address space ---- */
    uint64_t *user_pml4 = vmm_create_address_space();
    if (!user_pml4) {
        printf("exec: failed to create address space\n");
        kfree(image);
        return -1;
    }

    /* ---- Step 3: Map each PT_LOAD segment into user address space ---- */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        uint64_t seg_vaddr = ph->p_vaddr & ~0xFFFULL;       /* page-align start */
        uint64_t seg_vend  = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        uint64_t num_pages = (seg_vend - seg_vaddr) / PAGE_SIZE;

        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (ph->p_flags & 0x2) flags |= VMM_WRITABLE;       /* PF_W */
        if (!(ph->p_flags & 0x1)) flags |= VMM_NX;          /* !PF_X */

        for (uint64_t p = 0; p < num_pages; p++) {
            /* Zeroed frame, or the frame already backing this page when a
             * previous segment ends inside it (static non-PIE layouts). */
            if (exec_map_segment_page(user_pml4, seg_vaddr + p * PAGE_SIZE, flags) != 0) {
                printf("exec: out of memory mapping page at 0x%llx\n",
                       (unsigned long long)(seg_vaddr + p * PAGE_SIZE));
                vmm_destroy_address_space(user_pml4);
                kfree(image);
                return -1;
            }
        }

        /* Copy file data into the mapped pages (through HHDM mapping).
         * We need the physical address of each page to write via HHDM. */
        if (ph->p_filesz > 0) {
            uint8_t *src = (uint8_t *)image + ph->p_offset;
            uint64_t copy_vaddr = ph->p_vaddr;
            uint64_t remaining  = ph->p_filesz;

            while (remaining > 0) {
                uint64_t page_vaddr = copy_vaddr & ~0xFFFULL;
                uint64_t page_off   = copy_vaddr & 0xFFF;
                uint64_t phys = vmm_is_mapped_in(user_pml4, page_vaddr);
                if (!phys) break;

                uint64_t chunk = PAGE_SIZE - page_off;
                if (chunk > remaining) chunk = remaining;

                memcpy((uint8_t *)phys_to_virt(phys) + page_off, src, chunk);
                src        += chunk;
                copy_vaddr += chunk;
                remaining  -= chunk;
            }
        }
    }
    exec_dbg_mark("PT_LOAD mapped");

    /* ---- Step 3b: Handle dynamic relocations ---- */
    Elf64_Phdr *dyn_ph = NULL;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_DYNAMIC) { dyn_ph = ph; break; }
    }

    if (dyn_ph) {
        exec_apply_relocations(user_pml4, dyn_ph);
    }
    exec_dbg_mark("relocations done");

    /* ---- Step 4: Allocate user stack ---- */
    /* We allocate USER_STACK_PAGES + 1 so that the page AT USER_STACK_TOP is
     * also mapped.  The x86-64 ABI places argc/argv/envp at [initial_RSP],
     * [initial_RSP+8], etc., so that top page must be accessible. */
    uint64_t ustack_top  = USER_STACK_TOP;
    uint64_t ustack_base = ustack_top - (USER_STACK_PAGES * PAGE_SIZE);

    for (uint64_t p = 0; p <= USER_STACK_PAGES; p++) {   /* +1: includes top page */
        uint64_t vaddr = (p == USER_STACK_PAGES)
            ? ustack_top                                  /* the extra top page */
            : ustack_base + p * PAGE_SIZE;
        uint64_t frame = vmm_alloc_frame();
        if (!frame) {
            printf("exec: out of frames for user stack\n");
            vmm_destroy_address_space(user_pml4);
            kfree(image);
            return -1;
        }
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        vmm_map_page_in(user_pml4, vaddr, frame,
                        VMM_PRESENT | VMM_USER | VMM_WRITABLE | VMM_NX);
    }

    /* ---- Step 4b: Build the Linux x86-64 ABI initial stack frame ----
     *
     * The process entry point expects this layout at [RSP]:
     *
     *   [RSP +  0]  argc              (8 bytes)
     *   [RSP +  8]  argv[0]           pointer to program name string
     *   [RSP + 16]  argv[1] = NULL    end of argv
     *   [RSP + 24]  envp[0] = NULL    empty environment
     *   [RSP + 32]  AT_NULL  (tag)    end of aux vector
     *   [RSP + 40]  AT_NULL  (val)
     *
     * We write the program name string at the very top of the stack page
     * and build the table 48 bytes below that, aligning RSP to 16 bytes
     * per the ABI.
     *
     * Everything is written via the HHDM so that it ends up in the correct
     * physical frames and is readable by user code after the address-space
     * switch.
     */
    exec_collect_auxinfo(image);
    ustack_top = exec_build_abi_stack(user_pml4, ustack_top, name);
    exec_dbg_mark("user stack + abi frame");
    exec_dbg_mark("user stack + abi frame");

    /* ---- Step 5: Allocate per-process kernel stack ---- */
    uint8_t *kstack = (uint8_t *)kalloc(KSTACK_SIZE);
    if (!kstack) {
        printf("exec: failed to allocate kernel stack\n");
        vmm_destroy_address_space(user_pml4);
        kfree(image);
        return -1;
    }
    exec_dbg_mark("kernel stack allocated");
    uint64_t kstack_top = (uint64_t)kstack + KSTACK_SIZE;

    /* ---- Step 6: Set up process_t ---- */
    process_t *proc = process_alloc();
    if (!proc) {
        printf("exec: no free process slots\n");
        kfree(kstack);
        vmm_destroy_address_space(user_pml4);
        kfree(image);
        return -1;
    }
    exec_dbg_mark("process slot allocated");

    proc->pml4            = user_pml4;
    proc->kernel_stack    = kstack;
    proc->kernel_stack_top = kstack_top;
    proc->user_stack_top  = ustack_top;
    proc->memory_base     = NULL;  /* not used for Ring 3 */
    proc->virtual_base    = seg_base;
    proc->memory_size     = seg_end - seg_base;
    proc->brk_start       = (void *)((seg_end + 0xFFF) & ~0xFFFULL);
    proc->brk_current     = proc->brk_start;
    /* Anonymous mmap area starts EXEC_MMAP_GAP above the break so a growing
     * brk heap (musl, Doom's zone) cannot run into mmap'd pages. */
    proc->mmap_anon_next  = ((uint64_t)proc->brk_current + EXEC_MMAP_GAP + PAGE_SIZE - 1) &
                            ~(uint64_t)(PAGE_SIZE - 1);
    proc->is_usermode     = true;
    proc->linux_abi       = false;
    proc->linux_ti_valid  = false;
    proc->linux_clear_child_tid = 0;
    if (name && name[0]) {
        kstrncpy(proc->linux_exe_path, name, sizeof(proc->linux_exe_path));
    } else {
        proc->linux_exe_path[0] = 0;
    }
    proc->parent          = get_current_process();

    {
        process_t *par = proc->parent;
        if (par && par->cont_tty >= 0 && par->cont_tty < MAX_VCONSOLES) {
            proc->cont_tty = par->cont_tty;
        } else {
            proc->cont_tty = (int8_t)vconsole_get_active();
        }
    }

    /* Link into parent's child list */
    if (proc->parent) {
        proc->sibling       = proc->parent->child;
        proc->parent->child = proc;
    }

    /* Keep caller's open fds (pipes after dup2) across execve. */
    if (proc->parent)
        process_inherit_fds(proc, proc->parent);

    exec_load_elf_symbols(image, size, proc);
    exec_dbg_mark("symbols loaded");
    uint64_t entry_rip = eh->e_entry;
    ktrace("exec: launching '%s' in Ring 3 (PID %d, entry 0x%llx)\n",
           name, (int)proc->pid, (unsigned long long)entry_rip);

    /* Free the file image buffer -- data is already copied into user pages */
    kfree(image);

    /* ---- Step 7: Switch context and enter user mode ---- */
    set_current_process(proc);

    /* Save the original TSS kernel stack so we can restore it on exit */
    exec_orig_kstack_top    = gdt_get_kernel_stack();
    exec_kernel_pml4_save   = vmm_get_kernel_pml4();

    /* Save kernel context.  exec_setjmp returns 0 on the first call and
     * non-zero (the exit status) when exec_longjmp fires from sys_exit. */
    int exit_val = exec_setjmp(&exec_return_ctx);
    if (exit_val == 0) {
        /* ---- First call: enter user mode ---- */
        gdt_set_kernel_stack(kstack_top);
        syscall_set_kernel_stack(kstack_top);
        vmm_switch_address_space(user_pml4);
        exec_ring3_active_proc = proc;
        exec_sync_thread = scheduler_get_current_thread();
        set_program_running(true);
        set_terminate_flag(false);
        ktrace("exec: entering Ring 3 rip=0x%llx rsp=0x%llx\n",
                      (unsigned long long)entry_rip, (unsigned long long)ustack_top);
        exec_dbg_mark("enter_usermode");
        /* Fresh program: no TLS until it calls arch_prctl(ARCH_SET_FS). */
        thread_wrfsbase(0);
        enter_usermode(entry_rip, ustack_top);
        __builtin_unreachable();
    }

    exec_dbg_mark("returned from user mode");
    /* ---- We arrive here when the user process called sys_exit ---- */
    /* exec_notify_exit / exec_force_terminate has already:
     *   - switched back to kernel PML4
     *   - restored the original TSS kernel stack
     * Re-enable interrupts in case we came from a timer ISR longjmp. */
    __asm__ volatile("sti" ::: "memory");
    exec_ring3_active_proc = NULL;
    exec_sync_thread = NULL;
    set_program_running(false);
    set_terminate_flag(false);
    keyboard_set_raw_mode(false);

    /* The app owned the framebuffer.  When the desktop compositor is running it
     * owns the display, so ask it for a full repaint; redrawing the console
     * here would paste the pre-desktop console screen over the desktop. */
    {
        extern bool desktop_is_running(void);
        extern void desktop_force_full_repaint(void);
        if (desktop_is_running()) {
            desktop_force_full_repaint();
        } else {
            fb_console_redraw_from_chargrid();
            fb_present_scanout();
        }
    }

    int actual_exit = exec_exit_status_val;
    ktrace("exec: process '%s' exited (status %d)\n", name, actual_exit);

    /* Restore kernel process as current */
    process_t *parent = proc->parent;

    /* Free user-mode resources before process_cleanup() clears the slot */
    exec_free_user_symbols(proc);
    process_cleanup(proc);
    if (parent) set_current_process(parent);

    vmm_destroy_address_space(user_pml4);
    kfree(kstack);

    return actual_exit;
}

/* ==================================================================
 * exec_spawn — non-blocking ELF launch via the preemptive scheduler
 * ==================================================================
 * Re-uses the same ELF loading logic as exec_run_elf (steps 1–6) but
 * instead of calling enter_usermode() + longjmp, creates a thread_t
 * whose saved_regs describe a Ring-3 iretq frame.  The timer-driven
 * preemptive_schedule() in idt.c picks it up automatically.
 */

/* User segment selectors (must match GDT in gdt.c / syscallasm.asm) */
#define SPAWN_USER_CS  0x23   /* 0x20 | RPL=3 */
#define SPAWN_USER_SS  0x1B   /* 0x18 | RPL=3 */

/* ==================================================================
 * exec_replace — in-place execve for an already-running usermode process
 * ==================================================================
 * Tears down the old user address space, loads a new ELF into a fresh
 * PML4, keeps the same pid / parent / kernel stack / non-CLOEXEC fds,
 * resets caught signal handlers to SIG_DFL, and returns the new entry
 * (rip/rsp) via proc->exec_entry_* so the SYSCALL return path can
 * SYSRET into the new image (no nested exec_file / no new process slot).
 *
 * Must NOT free or replace proc->kernel_stack: we are executing on it.
 */
int exec_replace(process_t *proc, const char *filename, char *const argv[]) {
    if (!proc || !filename)
        return -EINVAL;
    if (!proc->is_usermode || !proc->pml4)
        return -EINVAL;

    exec_current_argv = argv;
    exec_dbg_reset();
    exec_dbg_mark("exec_replace");

    if (!vfs_has_root_mount() && !fat32_is_mounted())
        return -ENOENT;

    char abs_path[256];
    const char *resolved = filename;
    if (filename[0] != '/') {
        abs_path[0] = '/';
        int i = 1;
        for (int j = 0; filename[j] && i < 255; j++)
            abs_path[i++] = filename[j];
        abs_path[i] = 0;
        resolved = abs_path;
    }

    void *image = NULL;
    uint32_t fsize = 0;
    int bytes_read = 0;
    if (vfs_has_root_mount()) {
        int n = vfs_read_file_alloc(resolved, &image, 0);
        if (n <= 0 || !image) return -ENOENT;
        fsize = (uint32_t)n;
        bytes_read = n;
    } else {
        fat32_file_t *file = fat32_open(resolved, "r");
        if (!file) return -ENOENT;
        fsize = file->file_size;
        if (fsize == 0) { fat32_close(file); return -ENOENT; }
        image = kalloc(fsize);
        if (!image) { fat32_close(file); return -ENOMEM; }
        bytes_read = fat32_read(file, image, fsize);
        fat32_close(file);
    }
    if (bytes_read != (int)fsize) {
        kfree(image);
        return -EIO;
    }
    if (fsize < sizeof(Elf64_Ehdr) || memcmp(image, ELF_MAGIC, 4) != 0) {
        kfree(image);
        return -ENOEXEC;
    }
    {
        const char *why = exec_elf_unsupported(image, fsize);
        if (why) {
            serial_printf("exec_replace: %s: %s\n", resolved, why);
            kfree(image);
            return -ENOEXEC;
        }
    }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)image;
    uint64_t seg_base = UINT64_MAX, seg_end = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_filesz == 0 && ph->p_memsz == 0) continue;
        if (ph->p_vaddr < seg_base) seg_base = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > seg_end) seg_end = ph->p_vaddr + ph->p_memsz;
    }
    if (seg_base == UINT64_MAX) { kfree(image); return -ENOEXEC; }

    uint64_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) { kfree(image); return -ENOMEM; }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        uint64_t sv = ph->p_vaddr & ~0xFFFULL;
        uint64_t se = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        uint64_t np = (se - sv) / PAGE_SIZE;
        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (ph->p_flags & 0x2) flags |= VMM_WRITABLE;
        if (!(ph->p_flags & 0x1)) flags |= VMM_NX;
        for (uint64_t pg = 0; pg < np; pg++) {
            if (exec_map_segment_page(new_pml4, sv + pg * PAGE_SIZE, flags) != 0) {
                vmm_destroy_address_space(new_pml4);
                kfree(image);
                return -ENOMEM;
            }
        }
        if (ph->p_filesz > 0) {
            uint8_t *src = (uint8_t *)image + ph->p_offset;
            uint64_t cv = ph->p_vaddr;
            uint64_t rem = ph->p_filesz;
            while (rem > 0) {
                uint64_t pv = cv & ~0xFFFULL;
                uint64_t po = cv & 0xFFF;
                uint64_t phys = vmm_is_mapped_in(new_pml4, pv);
                if (!phys) break;
                uint64_t chunk = PAGE_SIZE - po;
                if (chunk > rem) chunk = rem;
                memcpy((uint8_t *)phys_to_virt(phys) + po, src, chunk);
                src += chunk; cv += chunk; rem -= chunk;
            }
        }
    }

    Elf64_Phdr *dyn_ph = NULL;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_DYNAMIC) { dyn_ph = ph; break; }
    }
    if (dyn_ph)
        exec_apply_relocations(new_pml4, dyn_ph);

    uint64_t ustack_top  = USER_STACK_TOP;
    uint64_t ustack_base = ustack_top - (USER_STACK_PAGES * PAGE_SIZE);
    for (uint64_t pg = 0; pg <= USER_STACK_PAGES; pg++) {
        uint64_t vaddr = (pg == USER_STACK_PAGES) ? ustack_top : ustack_base + pg * PAGE_SIZE;
        uint64_t frame = vmm_alloc_frame();
        if (!frame) {
            vmm_destroy_address_space(new_pml4);
            kfree(image);
            return -ENOMEM;
        }
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        vmm_map_page_in(new_pml4, vaddr, frame,
                        VMM_PRESENT | VMM_USER | VMM_WRITABLE | VMM_NX);
    }
    exec_collect_auxinfo(image);
    ustack_top = exec_build_abi_stack(new_pml4, ustack_top, resolved);
    uint64_t entry_rip = eh->e_entry;

    /* Tear down old user AS — switch to kernel first so we don't free CR3. */
    extern uint64_t *vmm_get_kernel_pml4(void);
    vmm_switch_address_space(vmm_get_kernel_pml4());
    if (proc->pml4) {
        vmm_destroy_address_space(proc->pml4);
        proc->pml4 = NULL;
    }
    while (proc->mmap_regions) {
        proc_mmap_region_t *r = proc->mmap_regions;
        proc->mmap_regions = r->next;
        kfree(r);
    }
    exec_free_user_symbols(proc);

    proc->pml4           = new_pml4;
    proc->user_stack_top = ustack_top;
    proc->virtual_base   = seg_base;
    proc->memory_size    = seg_end - seg_base;
    proc->memory_base    = NULL;
    proc->brk_start      = (void *)((seg_end + 0xFFF) & ~0xFFFULL);
    proc->brk_current    = proc->brk_start;
    proc->mmap_anon_next = ((uint64_t)proc->brk_current + EXEC_MMAP_GAP + PAGE_SIZE - 1) &
                           ~(uint64_t)(PAGE_SIZE - 1);
    proc->linux_ti_valid = false;
    proc->linux_clear_child_tid = 0;
    kstrncpy(proc->linux_exe_path, resolved, sizeof(proc->linux_exe_path));

    /* Reset caught signal handlers to default; leave SIG_IGN alone.
     * Also clear sa_restorer — stale ash trampolines must not survive exec
     * into another applet of the same busybox binary. */
    for (int s = 1; s < NSIG; s++) {
        if (proc->signal_handlers[s] != SIG_IGN)
            proc->signal_handlers[s] = SIG_DFL;
        proc->signal_sa_flags[s] = 0;
        proc->sig_restorer[s] = 0;
    }
    proc->pending_signals = 0;
    proc->sig_in_handler = false;

    exec_load_elf_symbols(image, fsize, proc);
    kfree(image);

    /* Install new address space for this process. */
    vmm_switch_address_space(new_pml4);
    if (proc->kernel_stack_top) {
        gdt_set_kernel_stack(proc->kernel_stack_top);
        syscall_set_kernel_stack(proc->kernel_stack_top);
    }
    exec_set_ring3_active(proc);
    thread_wrfsbase(0);
    if (proc->sched_thread) {
        thread_t *thr = (thread_t *)proc->sched_thread;
        thr->fs_base = 0;
        memset(&thr->saved_regs, 0, sizeof(thread_regs_t));
        thr->saved_regs.rip    = entry_rip;
        thr->saved_regs.rsp    = ustack_top;
        thr->saved_regs.rflags = 0x202;
        thr->saved_regs.cs     = 0x23;
        thr->saved_regs.ss     = 0x1B;
        thr->has_saved_context = true;
    }

    proc->exec_entry_rip = entry_rip;
    proc->exec_entry_rsp = ustack_top;
    proc->exec_replaced  = true;

    {
        const char *a0 = (argv && argv[0]) ? argv[0] : "(null)";
        const char *a1 = (argv && argv[0] && argv[1]) ? argv[1] : "";
        const char *a2 = (argv && argv[0] && argv[1] && argv[2]) ? argv[2] : "";
        serial_printf("exec_replace: PID %d -> '%s' argv0='%s' argv1='%s' argv2='%s' entry=0x%llx rsp=0x%llx\n",
                      (int)proc->pid, resolved, a0, a1, a2,
                      (unsigned long long)entry_rip,
                      (unsigned long long)ustack_top);
        {
            const char *tn[3] = {"?", "?", "?"};
            for (int i = 0; i < 3; i++) {
                if (!proc->fds[i].in_use) { tn[i] = "closed"; continue; }
                switch (proc->fds[i].fd_type) {
                case 1: tn[i] = "pipe_r"; break;
                case 2: tn[i] = "pipe_w"; break;
                case 6: tn[i] = "null"; break;
                case 7: tn[i] = "tty"; break;
                case 0: tn[i] = "file/cons"; break;
                default: tn[i] = "other"; break;
                }
            }
            serial_printf("exec_fds: pid=%d fd0=%s fd1=%s fd2=%s\n",
                          (int)proc->pid, tn[0], tn[1], tn[2]);
        }
    }
    return 0;
}

int exec_spawn(const char *filename, char *const argv[]) {
    exec_current_argv = argv;
    exec_dbg_reset();
    exec_dbg_mark(filename);

    /* ---- Read file ---- */
    if (!vfs_has_root_mount() && !fat32_is_mounted()) return -1;

    char abs_path[256];
    const char *resolved = filename;
    if (filename[0] != '/') {
        abs_path[0] = '/';
        int i = 1;
        for (int j = 0; filename[j] && i < 255; j++)
            abs_path[i++] = filename[j];
        abs_path[i] = 0;
        resolved = abs_path;
    }

    void *image = NULL;
    uint32_t fsize = 0;
    int bytes_read = 0;

    if (vfs_has_root_mount()) {
        int n = vfs_read_file_alloc(resolved, &image, 0);
        if (n <= 0 || !image) return -1;
        fsize = (uint32_t)n;
        bytes_read = n;
        exec_dbg_mark("file read complete");
    } else {
        fat32_file_t *file = fat32_open(resolved, "r");
        if (!file) return -1;
        fsize = file->file_size;
        if (fsize == 0) { fat32_close(file); return -1; }
        image = kalloc(fsize);
        if (!image) { fat32_close(file); return -1; }
        bytes_read = fat32_read(file, image, fsize);
        fat32_close(file);
    }
    if (bytes_read != (int)fsize) {
        ktrace("exec_spawn: '%s' read mismatch (%d vs %u)\n",
                      filename, bytes_read, fsize);
        kfree(image);
        return -1;
    }

    serial_printf("exec_spawn: '%s' loaded %u bytes into memory\n",
                  filename, fsize);

    /* ---- Validate ELF ---- */
    if (fsize < sizeof(Elf64_Ehdr) || memcmp(image, ELF_MAGIC, 4) != 0) {
        serial_printf("exec_spawn: '%s' not a valid ELF (magic: %02x%02x%02x%02x)\n",
                      filename,
                      ((uint8_t *)image)[0], ((uint8_t *)image)[1],
                      ((uint8_t *)image)[2], ((uint8_t *)image)[3]);
        kfree(image);
        return -1;
    }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)image;
    if (eh->e_phoff == 0 || eh->e_phnum == 0) {
        serial_printf("exec_spawn: '%s' ELF has no program headers\n", filename);
        kfree(image);
        return -1;
    }
    {
        const char *why = exec_elf_unsupported(image, fsize);
        if (why) {
            serial_printf("exec_spawn: '%s': %s\n", filename, why);
            kfree(image);
            return -1;
        }
    }

    /* ---- Step 1: Find loadable segment range ---- */
    uint64_t seg_base = UINT64_MAX, seg_end = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_filesz == 0 && ph->p_memsz == 0) continue;
        if (ph->p_vaddr < seg_base) seg_base = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > seg_end) seg_end = ph->p_vaddr + ph->p_memsz;
    }
    if (seg_base == UINT64_MAX) { kfree(image); return -1; }
    exec_dbg_mark("segments scanned");

    /* ---- Step 2: Create user address space ---- */
    uint64_t *user_pml4 = vmm_create_address_space();
    if (!user_pml4) { kfree(image); return -1; }

    /* ---- Step 3: Map PT_LOAD segments ---- */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t sv = ph->p_vaddr & ~0xFFFULL;
        uint64_t se = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        uint64_t np = (se - sv) / PAGE_SIZE;
        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (ph->p_flags & 0x2) flags |= VMM_WRITABLE;
        if (!(ph->p_flags & 0x1)) flags |= VMM_NX;

        for (uint64_t p = 0; p < np; p++) {
            if (exec_map_segment_page(user_pml4, sv + p * PAGE_SIZE, flags) != 0) {
                vmm_destroy_address_space(user_pml4); kfree(image); return -1;
            }
        }

        if (ph->p_filesz > 0) {
            uint8_t *src = (uint8_t *)image + ph->p_offset;
            uint64_t cv = ph->p_vaddr;
            uint64_t rem = ph->p_filesz;
            while (rem > 0) {
                uint64_t pv = cv & ~0xFFFULL;
                uint64_t po = cv & 0xFFF;
                uint64_t phys = vmm_is_mapped_in(user_pml4, pv);
                if (!phys) break;
                uint64_t chunk = PAGE_SIZE - po;
                if (chunk > rem) chunk = rem;
                memcpy((uint8_t *)phys_to_virt(phys) + po, src, chunk);
                src += chunk; cv += chunk; rem -= chunk;
            }
        }
    }
    exec_dbg_mark("PT_LOAD mapped");

    /* ---- Step 3b: Handle dynamic relocations ---- */
    Elf64_Phdr *dyn_ph = NULL;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_DYNAMIC) { dyn_ph = ph; break; }
    }
    if (dyn_ph) {
        exec_apply_relocations(user_pml4, dyn_ph);
    }
    exec_dbg_mark("relocations done");

    /* ---- Step 4: Allocate user stack ---- */
    uint64_t ustack_top  = USER_STACK_TOP;
    uint64_t ustack_base = ustack_top - (USER_STACK_PAGES * PAGE_SIZE);
    for (uint64_t p = 0; p <= USER_STACK_PAGES; p++) {
        uint64_t vaddr = (p == USER_STACK_PAGES) ? ustack_top : ustack_base + p * PAGE_SIZE;
        uint64_t frame = vmm_alloc_frame();
        if (!frame) { vmm_destroy_address_space(user_pml4); kfree(image); return -1; }
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        vmm_map_page_in(user_pml4, vaddr, frame,
                        VMM_PRESENT | VMM_USER | VMM_WRITABLE | VMM_NX);
    }

    exec_collect_auxinfo(image);
    ustack_top = exec_build_abi_stack(user_pml4, ustack_top, filename);

    /* ---- Step 5: Allocate per-process kernel stack ---- */
    uint8_t *kstack = (uint8_t *)kalloc(KSTACK_SIZE);
    if (!kstack) { vmm_destroy_address_space(user_pml4); kfree(image); return -1; }
    uint64_t kstack_top = (uint64_t)kstack + KSTACK_SIZE;

    /* ---- Step 6: Set up process_t ---- */
    process_t *proc = process_alloc();
    if (!proc) { kfree(kstack); vmm_destroy_address_space(user_pml4); kfree(image); return -1; }

    proc->pml4             = user_pml4;
    proc->kernel_stack     = kstack;
    proc->kernel_stack_top = kstack_top;
    proc->user_stack_top   = ustack_top;
    proc->virtual_base     = seg_base;
    proc->memory_size      = seg_end - seg_base;
    proc->brk_start        = (void *)((seg_end + 0xFFF) & ~0xFFFULL);
    proc->brk_current      = proc->brk_start;
    proc->mmap_anon_next   = ((uint64_t)proc->brk_current + EXEC_MMAP_GAP + PAGE_SIZE - 1)
                             & ~(uint64_t)(PAGE_SIZE - 1);
    proc->is_usermode      = true;
    proc->linux_abi        = false;
    proc->linux_ti_valid   = false;
    proc->linux_clear_child_tid = 0;
    if (filename && filename[0])
        kstrncpy(proc->linux_exe_path, filename, sizeof(proc->linux_exe_path));
    else
        proc->linux_exe_path[0] = 0;
    proc->parent           = get_current_process();
    {
        process_t *par = proc->parent;
        if (par && par->cont_tty >= 0 && par->cont_tty < 4)
            proc->cont_tty = par->cont_tty;
        else {
            extern int vconsole_get_active(void);
            proc->cont_tty = (int8_t)vconsole_get_active();
        }
    }
    if (proc->parent) {
        proc->sibling       = proc->parent->child;
        proc->parent->child = proc;
    }

    exec_load_elf_symbols(image, fsize, proc);
    exec_dbg_mark("symbols loaded");
    uint64_t entry_rip = eh->e_entry;
    kfree(image);

    /* ---- Step 7: Create scheduled thread for Ring-3 execution ---- */
    extern void thread_exit(void);
    thread_t *thr = thread_create(filename, (void(*)(void*))thread_exit,
                                  NULL, 0, PRIORITY_NORMAL);
    if (!thr) {
        process_cleanup(proc);
        vmm_destroy_address_space(user_pml4);
        kfree(kstack);
        return -1;
    }

    /* Override saved_regs with Ring-3 iretq context */
    memset(&thr->saved_regs, 0, sizeof(thread_regs_t));
    thr->saved_regs.rip    = entry_rip;
    thr->saved_regs.cs     = SPAWN_USER_CS;
    thr->saved_regs.rflags = 0x202;       /* IF set */
    thr->saved_regs.rsp    = ustack_top;
    thr->saved_regs.ss     = SPAWN_USER_SS;
    thr->has_saved_context = true;
    thr->owner_process     = (struct process *)proc;

    proc->sched_thread = thr;

    scheduler_add_thread(thr);
    exec_dbg_mark("thread queued");

    /* Do NOT hand off with thread_yield() here.
     *
     * A cooperative switch resumes a thread at thread->rip through
     * thread_start_asm, and this thread's cooperative entry point is
     * thread_exit (see thread_create above).  The Ring-3 entry point lives in
     * saved_regs and is only reachable when the timer ISR restores that iretq
     * frame.  Yielding here therefore ran thread_exit straight away: every
     * 'spawn'ed app (and every dock click, which uses this function) exited
     * within microseconds and never entered Ring 3.
     *
     * exec_spawn is designed around preemptive_schedule() picking the thread up
     * on the next quantum (<= ~10 ms), so just make sure interrupts are on. */
    if (!in_interrupt_context()) {
        irq_enable();
    }

    int pid = (int)proc->pid;
    serial_printf("exec_spawn: '%s' PID %d entry=0x%llx  rsp=0x%llx\n",
                  filename, pid,
                  (unsigned long long)entry_rip,
                  (unsigned long long)ustack_top);
    return pid;
}

/* ==================================================================
 * exec_spawned_exit — exit path for preemptively-scheduled processes
 * ==================================================================
 * Called from sys_exit when the current process has sched_thread set.
 * Cleans up and halts; the timer ISR + preemptive_schedule picks the
 * next runnable thread.
 */
/* Historical: SYSCALL entry used to leave GS swapped.  It no longer does,
 * so exec_spawned_exit never swapgs; the flag and the _int80 variant are kept
 * so existing callers (desktop.c) keep compiling. */
static bool exec_exit_undo_swapgs = true;

__attribute__((noreturn))
void exec_spawned_exit_int80(int status) {
    exec_exit_undo_swapgs = false;
    exec_spawned_exit(status);
}

__attribute__((noreturn))
void exec_spawned_exit(int status) {
    process_t *proc = get_current_process();
    thread_t  *thr  = (thread_t *)proc->sched_thread;

    serial_printf("exec_spawned_exit: PID %d status=%d\n",
                  (int)proc->pid, status);

    proc->exit_status = status;
    proc->is_running  = false;
    /* Spawned processes are launched by the in-kernel shell/desktop, which
     * never calls sys_wait(). Don't leave them as permanent zombies holding
     * a process slot — they are fully reaped below via process_cleanup(). */
    proc->is_zombie   = false;

    /* Destroy desktop windows owned by this process */
    desktop_destroy_windows_for_pid(proc->pid);

    /* Switch back to kernel address space */
    extern uint64_t *vmm_get_kernel_pml4(void);
    vmm_switch_address_space(vmm_get_kernel_pml4());

    /* Restore original TSS kernel stack so timer ISR has a safe stack */
    extern uint64_t gdt_get_kernel_stack(void);
    uint64_t orig_kstack = gdt_get_kernel_stack();
    (void)orig_kstack; /* TSS RSP0 will be set by preemptive_schedule */

    /* GS is already in its invariant state on both entry paths: INT 0x80
     * never swaps, and syscall_entry swaps back before calling C.  (This
     * used to swapgs here for the SYSCALL case.) */
    exec_exit_undo_swapgs = true;

    /* Free user address space (safe: we already switched to kernel PML4) */
    if (proc->pml4) {
        vmm_destroy_address_space(proc->pml4);
        proc->pml4 = NULL;
    }

    exec_free_user_symbols(proc);
    keyboard_restore_shell_input();

    /* App drew over the framebuffer (e.g. dvd.exe); restore the shell — unless
     * the desktop owns the display, in which case repaint the desktop. */
    {
        extern bool desktop_is_running(void);
        extern void desktop_force_full_repaint(void);
        if (desktop_is_running()) {
            desktop_force_full_repaint();
        } else {
            fb_console_redraw_from_chargrid();
            fb_present_scanout();
        }
    }
    printf("%s", tty_get_prompt());

    /* Mark thread terminated so the scheduler skips it */
    if (thr) {
        thr->state = THREAD_TERMINATED;
        thr->owner_process = NULL; /* slot is reaped below; avoid dangling */
        scheduler_detach_thread(thr);
    }

    /* Fully reap: close fds, unlink from parent, release the process slot.
     * current_process must not point at the cleared slot afterwards. */
    extern process_t *get_kernel_process(void);
    process_t *fallback = proc->parent ? proc->parent : get_kernel_process();
    process_cleanup(proc);
    set_current_process(fallback);

    /* Halt with interrupts enabled; timer will fire and scheduler picks next. */
    __asm__ volatile("sti");
    for (;;)
        __asm__ volatile("hlt");
}