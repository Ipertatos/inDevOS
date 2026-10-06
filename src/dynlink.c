/**
 * @file dynlink.c
 * @brief Dynamic linker / shared library loader implementation
 */

#include "dynlink.h"
#include "fat32.h"
#include "mem.h"
#include "serial.h"
#include "string.h"

extern int  printf(const char *fmt, ...);
extern int  snprintf(char *str, size_t size, const char *fmt, ...);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
extern int   memcmp(const void *, const void *, size_t);
extern int   strcmp(const char *a, const char *b);
extern int   strncmp(const char *a, const char *b, size_t n);
extern size_t strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, size_t n);

/* ================================================================
 *  ELF header (minimal – just for parsing .so files)
 * ================================================================ */

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
} dl_elf64_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} dl_elf64_phdr_t;

typedef struct __attribute__((packed)) {
    uint64_t d_tag;
    uint64_t d_val;
} dl_elf64_dyn_t;

#define PT_LOAD_DL    1
#define PT_DYNAMIC_DL 2
#define ET_DYN        3   /* shared object */

static const uint8_t ELF_MAGIC_DL[4] = {0x7F, 'E', 'L', 'F'};

/* ================================================================
 *  Global state
 * ================================================================ */

static dynlink_lib_t    loaded_libs[DYNLINK_MAX_LIBS];
static dynlink_symbol_t global_symbols[DYNLINK_MAX_SYMBOLS];
static int              global_sym_count = 0;
static bool             dynlink_ready = false;

/* Base address for loading shared objects (incremented per load) */
static uint64_t next_load_base = 0x0000100000000000ULL;  /* well above user code/stack */

/* ================================================================
 *  Initialisation
 * ================================================================ */

void dynlink_init(void) {
    memset(loaded_libs, 0, sizeof(loaded_libs));
    memset(global_symbols, 0, sizeof(global_symbols));
    global_sym_count = 0;
    next_load_base   = 0x0000100000000000ULL;
    dynlink_ready    = true;
    serial_printf("dynlink: initialized\n");
}

/* ================================================================
 *  Symbol table management
 * ================================================================ */

int dynlink_register_symbol(const char *name, uint64_t address) {
    if (!name || global_sym_count >= DYNLINK_MAX_SYMBOLS) return -1;

    /* Check for duplicate */
    for (int i = 0; i < global_sym_count; i++) {
        if (strcmp(global_symbols[i].name, name) == 0) {
            global_symbols[i].address = address;
            return 0;
        }
    }

    strncpy(global_symbols[global_sym_count].name, name, DYNLINK_MAX_NAME_LEN - 1);
    global_symbols[global_sym_count].address   = address;
    global_symbols[global_sym_count].lib_index = -1;  /* kernel */
    global_symbols[global_sym_count].binding   = STB_GLOBAL;
    global_symbols[global_sym_count].type      = STT_FUNC;
    global_sym_count++;
    return 0;
}

uint64_t dynlink_resolve(const char *name) {
    if (!name) return 0;

    /* Search global symbol table */
    for (int i = 0; i < global_sym_count; i++) {
        if (strcmp(global_symbols[i].name, name) == 0)
            return global_symbols[i].address;
    }

    /* Search each loaded library's symbol table */
    for (int lib = 0; lib < DYNLINK_MAX_LIBS; lib++) {
        dynlink_lib_t *l = &loaded_libs[lib];
        if (!l->loaded || !l->symtab || !l->strtab) continue;

        for (uint32_t s = 0; s < l->sym_count; s++) {
            const dynlink_elf_sym_t *sym = &l->symtab[s];
            if (sym->st_name == 0 || sym->st_value == 0) continue;
            if (ELF64_ST_BIND(sym->st_info) == STB_LOCAL) continue;

            const char *sname = l->strtab + sym->st_name;
            if (strcmp(sname, name) == 0)
                return l->load_base + sym->st_value;
        }
    }

    return 0;
}

/* ================================================================
 *  Apply relocations to a loaded library
 * ================================================================ */

static int apply_rela(dynlink_lib_t *lib, const dynlink_elf_rela_t *rela, uint32_t count) {
    int unresolved = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t rtype = (uint32_t)ELF64_R_TYPE(rela[i].r_info);
        uint32_t rsym  = (uint32_t)ELF64_R_SYM(rela[i].r_info);
        uint64_t target_addr = lib->load_base + rela[i].r_offset;
        uint64_t *target = (uint64_t *)target_addr;

        switch (rtype) {
        case R_X86_64_RELATIVE_TYPE:
            /* B + A */
            *target = lib->load_base + rela[i].r_addend;
            break;

        case R_X86_64_64_TYPE:
        case R_X86_64_GLOB_DAT_TYPE:
        case R_X86_64_JUMP_SLOT_TYPE: {
            /* S + A  where S = symbol value */
            uint64_t sym_addr = 0;
            if (rsym > 0 && rsym < lib->sym_count && lib->symtab && lib->strtab) {
                const char *sym_name = lib->strtab + lib->symtab[rsym].st_name;
                sym_addr = dynlink_resolve(sym_name);
                if (sym_addr == 0 && lib->symtab[rsym].st_value != 0) {
                    /* Local definition */
                    sym_addr = lib->load_base + lib->symtab[rsym].st_value;
                }
                if (sym_addr == 0) {
                    serial_printf("dynlink: unresolved symbol '%s'\n", sym_name);
                    unresolved++;
                }
            }
            *target = sym_addr + rela[i].r_addend;
            break;
        }

        case R_X86_64_NONE:
            break;

        default:
            serial_printf("dynlink: unhandled relocation type %u\n", rtype);
            break;
        }
    }
    return unresolved ? -1 : 0;
}

/* ================================================================
 *  Load a shared library from FAT32
 * ================================================================ */

int dynlink_load(const char *path) {
    if (!dynlink_ready || !path) return -1;

    /* Check if already loaded */
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (loaded_libs[i].loaded && strcmp(loaded_libs[i].name, path) == 0) {
            loaded_libs[i].ref_count++;
            return i;
        }
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (!loaded_libs[i].loaded) { slot = i; break; }
    }
    if (slot < 0) {
        printf("dynlink: too many loaded libraries\n");
        return -1;
    }

    /* Read file from FAT32 */
    if (!fat32_is_mounted()) {
        printf("dynlink: no filesystem mounted\n");
        return -1;
    }

    fat32_file_t *f = fat32_open(path, "r");
    if (!f) {
        printf("dynlink: cannot open '%s'\n", path);
        return -1;
    }

    uint32_t fsize = f->file_size;
    if (fsize < sizeof(dl_elf64_ehdr_t)) {
        printf("dynlink: file too small\n");
        fat32_close(f);
        return -1;
    }

    void *image = kalloc(fsize);
    if (!image) {
        printf("dynlink: out of memory\n");
        fat32_close(f);
        return -1;
    }

    int bytes_read = fat32_read(f, image, fsize);
    fat32_close(f);
    if (bytes_read != (int)fsize) {
        printf("dynlink: read error\n");
        kfree(image);
        return -1;
    }

    /* Verify ELF header */
    dl_elf64_ehdr_t *eh = (dl_elf64_ehdr_t *)image;
    if (memcmp(eh->e_ident, ELF_MAGIC_DL, 4) != 0) {
        printf("dynlink: not an ELF file\n");
        kfree(image);
        return -1;
    }
    if (eh->e_type != ET_DYN) {
        printf("dynlink: not a shared object (type %u)\n", eh->e_type);
        kfree(image);
        return -1;
    }

    /* Determine load range */
    uint64_t seg_lo = UINT64_MAX, seg_hi = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        dl_elf64_phdr_t *ph = (dl_elf64_phdr_t *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD_DL) continue;
        if (ph->p_vaddr < seg_lo) seg_lo = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > seg_hi) seg_hi = ph->p_vaddr + ph->p_memsz;
    }
    if (seg_lo == UINT64_MAX) {
        printf("dynlink: no loadable segments\n");
        kfree(image);
        return -1;
    }

    /* Allocate memory at next_load_base */
    uint64_t load_size = seg_hi - seg_lo;
    uint64_t load_base = next_load_base;
    next_load_base += (load_size + 0xFFF) & ~0xFFFULL;
    next_load_base += 0x10000;  /* gap between libraries */

    uint8_t *mem = (uint8_t *)kalloc((uint32_t)load_size);
    if (!mem) {
        printf("dynlink: out of memory for %llu bytes\n", (unsigned long long)load_size);
        kfree(image);
        return -1;
    }
    memset(mem, 0, (uint32_t)load_size);

    /* For kernel-space loading, we use the actual kalloc'd address as the base */
    load_base = (uint64_t)mem;

    /* Copy PT_LOAD segments into allocated memory */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        dl_elf64_phdr_t *ph = (dl_elf64_phdr_t *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD_DL) continue;
        if (ph->p_filesz > 0) {
            uint64_t dst_off = ph->p_vaddr - seg_lo;
            memcpy(mem + dst_off, (uint8_t *)image + ph->p_offset, (uint32_t)ph->p_filesz);
        }
    }

    /* Initialize library descriptor */
    dynlink_lib_t *lib = &loaded_libs[slot];
    memset(lib, 0, sizeof(dynlink_lib_t));
    strncpy(lib->name, path, DYNLINK_MAX_NAME_LEN - 1);
    lib->load_base  = load_base;
    lib->load_size  = load_size;
    lib->image      = image;
    lib->image_size = fsize;
    lib->ref_count  = 1;
    lib->loaded     = true;

    /* Parse PT_DYNAMIC to find symbol table, string table, relocations */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        dl_elf64_phdr_t *ph = (dl_elf64_phdr_t *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_DYNAMIC_DL) continue;

        dl_elf64_dyn_t *dyn = (dl_elf64_dyn_t *)((uint8_t *)image + ph->p_offset);
        uint64_t strtab_off = 0, symtab_off = 0;
        uint64_t rela_off = 0, relasz = 0, relaent = 0;
        uint64_t jmprel_off = 0, pltrelsz = 0;
        uint64_t strsz = 0, syment = 0;
        uint64_t init_off = 0, fini_off = 0;

        for (; dyn->d_tag != DT_NULL; dyn++) {
            switch (dyn->d_tag) {
            case DT_STRTAB:    strtab_off = dyn->d_val; break;
            case DT_SYMTAB:    symtab_off = dyn->d_val; break;
            case DT_RELA_TAG:  rela_off = dyn->d_val; break;
            case DT_RELASZ_TAG: relasz = dyn->d_val; break;
            case DT_RELAENT_TAG: relaent = dyn->d_val; break;
            case DT_JMPREL:    jmprel_off = dyn->d_val; break;
            case DT_PLTRELSZ:  pltrelsz = dyn->d_val; break;
            case DT_STRSZ:     strsz = dyn->d_val; (void)strsz; break;
            case DT_SYMENT:    syment = dyn->d_val; break;
            case DT_INIT:      init_off = dyn->d_val; break;
            case DT_FINI:      fini_off = dyn->d_val; break;
            }
        }

        /* Map pointers into the loaded memory */
        if (strtab_off) lib->strtab = (const char *)(mem + strtab_off - seg_lo);
        if (symtab_off) lib->symtab = (const dynlink_elf_sym_t *)(mem + symtab_off - seg_lo);

        /* Estimate symbol count from the gap between symtab and strtab */
        if (symtab_off && strtab_off && syment) {
            lib->sym_count = (uint32_t)((strtab_off - symtab_off) / syment);
        }

        if (rela_off && relasz && relaent) {
            lib->rela = (const dynlink_elf_rela_t *)(mem + rela_off - seg_lo);
            lib->rela_count = (uint32_t)(relasz / relaent);
        }
        if (jmprel_off && pltrelsz) {
            lib->pltrel = (const dynlink_elf_rela_t *)(mem + jmprel_off - seg_lo);
            lib->pltrel_count = (uint32_t)(pltrelsz / sizeof(dynlink_elf_rela_t));
        }
        if (init_off) lib->init_func = load_base + init_off - seg_lo;
        if (fini_off) lib->fini_func = load_base + fini_off - seg_lo;
    }

    /* Export global symbols from this library */
    if (lib->symtab && lib->strtab) {
        for (uint32_t s = 0; s < lib->sym_count && global_sym_count < DYNLINK_MAX_SYMBOLS; s++) {
            const dynlink_elf_sym_t *sym = &lib->symtab[s];
            if (sym->st_name == 0 || sym->st_value == 0) continue;
            if (ELF64_ST_BIND(sym->st_info) == STB_LOCAL) continue;

            const char *sname = lib->strtab + sym->st_name;
            /* Don't overwrite existing stronger symbols */
            bool exists = false;
            for (int j = 0; j < global_sym_count; j++) {
                if (strcmp(global_symbols[j].name, sname) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                strncpy(global_symbols[global_sym_count].name, sname, DYNLINK_MAX_NAME_LEN - 1);
                global_symbols[global_sym_count].address   = load_base + sym->st_value - seg_lo;
                global_symbols[global_sym_count].lib_index = slot;
                global_symbols[global_sym_count].binding   = ELF64_ST_BIND(sym->st_info);
                global_symbols[global_sym_count].type      = ELF64_ST_TYPE(sym->st_info);
                global_sym_count++;
            }
        }
    }

    /* Apply relocations — fail closed on any unresolved symbol */
    if (lib->rela && lib->rela_count > 0) {
        if (apply_rela(lib, lib->rela, lib->rela_count) != 0) {
            kfree((void *)load_base);
            memset(lib, 0, sizeof(*lib));
            return -1;
        }
    }
    if (lib->pltrel && lib->pltrel_count > 0) {
        if (apply_rela(lib, lib->pltrel, lib->pltrel_count) != 0) {
            kfree((void *)load_base);
            memset(lib, 0, sizeof(*lib));
            return -1;
        }
    }

    /* Call DT_INIT if present */
    if (lib->init_func) {
        void (*init)(void) = (void (*)(void))lib->init_func;
        init();
    }

    serial_printf("dynlink: loaded '%s' at 0x%llx (%u bytes, %u symbols)\n",
                  path, (unsigned long long)load_base,
                  (unsigned)load_size, lib->sym_count);

    return slot;
}

/* ================================================================
 *  Unload
 * ================================================================ */

int dynlink_unload(int handle) {
    if (handle < 0 || handle >= DYNLINK_MAX_LIBS) return -1;
    dynlink_lib_t *lib = &loaded_libs[handle];
    if (!lib->loaded) return -1;

    lib->ref_count--;
    if (lib->ref_count > 0) return 0;

    /* Call DT_FINI if present */
    if (lib->fini_func) {
        void (*fini)(void) = (void (*)(void))lib->fini_func;
        fini();
    }

    /* Remove symbols belonging to this library */
    for (int i = global_sym_count - 1; i >= 0; i--) {
        if (global_symbols[i].lib_index == handle) {
            /* Shift remaining */
            for (int j = i; j < global_sym_count - 1; j++)
                global_symbols[j] = global_symbols[j + 1];
            global_sym_count--;
        }
    }

    /* Free memory */
    if (lib->image) kfree(lib->image);
    if (lib->load_base) kfree((void *)lib->load_base);

    memset(lib, 0, sizeof(dynlink_lib_t));
    return 0;
}

/* ================================================================
 *  Debug listing
 * ================================================================ */

void dynlink_list(void) {
    printf("=== Dynamic Linker Status ===\n");
    int loaded_count = 0;
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (!loaded_libs[i].loaded) continue;
        dynlink_lib_t *lib = &loaded_libs[i];
        printf("[%d] %s  base=0x%llx  size=%llu  syms=%u  refs=%d\n",
               i, lib->name,
               (unsigned long long)lib->load_base,
               (unsigned long long)lib->load_size,
               lib->sym_count,
               lib->ref_count);
        loaded_count++;
    }
    if (loaded_count == 0) printf("  No shared libraries loaded\n");

    printf("\nGlobal symbols: %d / %d\n", global_sym_count, DYNLINK_MAX_SYMBOLS);
    for (int i = 0; i < global_sym_count && i < 32; i++) {
        printf("  %-40s 0x%llx  (lib %d)\n",
               global_symbols[i].name,
               (unsigned long long)global_symbols[i].address,
               global_symbols[i].lib_index);
    }
    if (global_sym_count > 32)
        printf("  ... and %d more\n", global_sym_count - 32);
}

/* ================================================================
 *  dlopen / dlsym / dlclose wrappers
 * ================================================================ */

void *dl_open(const char *path, int flags) {
    (void)flags;
    int handle = dynlink_load(path);
    if (handle < 0) return NULL;
    return (void *)(uint64_t)(handle + 1);  /* 1-based to distinguish from NULL */
}

void *dl_sym(void *handle, const char *name) {
    if (!name) return NULL;

    if (handle) {
        int idx = (int)((uint64_t)handle - 1);
        if (idx >= 0 && idx < DYNLINK_MAX_LIBS && loaded_libs[idx].loaded) {
            /* Search this library first */
            dynlink_lib_t *lib = &loaded_libs[idx];
            if (lib->symtab && lib->strtab) {
                for (uint32_t s = 0; s < lib->sym_count; s++) {
                    const dynlink_elf_sym_t *sym = &lib->symtab[s];
                    if (sym->st_name == 0 || sym->st_value == 0) continue;
                    const char *sname = lib->strtab + sym->st_name;
                    if (strcmp(sname, name) == 0)
                        return (void *)(lib->load_base + sym->st_value);
                }
            }
        }
    }

    /* Fall back to global resolution */
    uint64_t addr = dynlink_resolve(name);
    return (void *)addr;
}

int dl_close(void *handle) {
    if (!handle) return -1;
    int idx = (int)((uint64_t)handle - 1);
    return dynlink_unload(idx);
}
