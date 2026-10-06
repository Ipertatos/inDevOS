/**
 * @file kmod.c
 * @brief Kernel Module (kmod) subsystem implementation
 *
 * Handles loading ELF relocatable object files as kernel modules.
 * Supports symbol resolution between the kernel and loaded modules,
 * and provides lifecycle management (init/cleanup).
 */

#include "kmod.h"
#include "mem.h"
#include "fat32.h"
#include "serial.h"
#include "utils.h"
#include "string.h"
#include "thread.h"

/* ============================================================================
 * ELF definitions (minimal set for relocatable objects)
 * ============================================================================ */

#define ELF_MAGIC       0x464C457F  /* "\x7FELF" as uint32_t */

/* ELF64 header */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  class;         /* 1=32-bit, 2=64-bit */
    uint8_t  data;          /* 1=LE, 2=BE */
    uint8_t  version;
    uint8_t  osabi;
    uint8_t  abiversion;
    uint8_t  pad[7];
    uint16_t type;          /* 1=REL, 2=EXEC, 3=DYN */
    uint16_t machine;       /* 0x3E = x86_64 */
    uint32_t version2;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;         /* Section header table offset */
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;         /* Number of section headers */
    uint16_t shstrndx;      /* Section name string table index */
} elf64_hdr_t;

/* ELF64 section header */
typedef struct __attribute__((packed)) {
    uint32_t name;          /* Offset into section name string table */
    uint32_t type;          /* SHT_* */
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;        /* Offset in file */
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
} elf64_shdr_t;

/* ELF64 symbol table entry */
typedef struct __attribute__((packed)) {
    uint32_t name;          /* Offset into string table */
    uint8_t  info;          /* Type and binding */
    uint8_t  other;
    uint16_t shndx;         /* Section index */
    uint64_t value;
    uint64_t size;
} elf64_sym_t;

/* ELF64 relocation entry with addend */
typedef struct __attribute__((packed)) {
    uint64_t offset;
    uint64_t info;          /* Symbol index + type */
    int64_t  addend;
} elf64_rela_t;

/* Section types */
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8   /* .bss */
#define SHT_REL         9

/* Section flags */
#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

/* Symbol binding (from st_info) */
#define ELF_ST_BIND(i)  ((i) >> 4)
#define ELF_ST_TYPE(i)  ((i) & 0xF)
#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STT_FUNC        2
#define STT_OBJECT      1

/* Relocation types for x86_64 */
#define R_X86_64_64         1   /* S + A */
#define R_X86_64_PC32       2   /* S + A - P */
#define R_X86_64_PLT32      4   /* S + A - P (PLT slot; same math for kmod) */
#define R_X86_64_32         10  /* S + A (32-bit) */
#define R_X86_64_32S        11  /* S + A (32-bit signed) */

#define ELF64_R_SYM(i)     ((i) >> 32)
#define ELF64_R_TYPE(i)    ((i) & 0xFFFFFFFF)

/* ============================================================================
 * Global state
 * ============================================================================ */

/** @brief Module table */
static kmod_t modules[KMOD_MAX_MODULES];

/** @brief Kernel exported symbol table */
#define KMOD_MAX_KERNEL_EXPORTS 256
static kmod_symbol_t kernel_exports[KMOD_MAX_KERNEL_EXPORTS];
static uint32_t num_kernel_exports = 0;

/** @brief Module subsystem initialized flag */
static bool kmod_initialized = false;

/* ============================================================================
 * Initialization
 * ============================================================================ */

void kmod_init(void) {
    memset(modules, 0, sizeof(modules));
    memset(kernel_exports, 0, sizeof(kernel_exports));
    num_kernel_exports = 0;

    /* Export some core kernel symbols that modules can use */
    kmod_export_symbol("printf",        (uint64_t)printf);
    kmod_export_symbol("serial_puts",   (uint64_t)serial_puts);
    kmod_export_symbol("serial_printf", (uint64_t)serial_printf);
    kmod_export_symbol("kalloc",        (uint64_t)kalloc);
    kmod_export_symbol("kfree",         (uint64_t)kfree);
    kmod_export_symbol("memset",        (uint64_t)memset);
    kmod_export_symbol("memcpy",        (uint64_t)memcpy);
    kmod_export_symbol("strlen",        (uint64_t)strlen);
    kmod_export_symbol("strcmp",        (uint64_t)strcmp);
    kmod_export_symbol("strcpy",        (uint64_t)strcpy);
    kmod_export_symbol("thread_create", (uint64_t)thread_create);
    kmod_export_symbol("thread_sleep",  (uint64_t)thread_sleep);
    kmod_export_symbol("scheduler_add_thread", (uint64_t)scheduler_add_thread);

    kmod_initialized = true;
    serial_puts("kmod: module subsystem initialized\n");
    printf("KMOD: Module subsystem initialized (%d kernel symbols exported)\n",
           num_kernel_exports);
}

/* ============================================================================
 * Symbol management
 * ============================================================================ */

int kmod_export_symbol(const char *name, uint64_t address) {
    if (!name || num_kernel_exports >= KMOD_MAX_KERNEL_EXPORTS) return -1;

    strncpy(kernel_exports[num_kernel_exports].name, name, 63);
    kernel_exports[num_kernel_exports].name[63] = '\0';
    kernel_exports[num_kernel_exports].address = address;
    num_kernel_exports++;

    return 0;
}

uint64_t kmod_resolve_symbol(const char *name) {
    if (!name) return 0;

    /* Search kernel exports first */
    for (uint32_t i = 0; i < num_kernel_exports; i++) {
        if (strcmp(kernel_exports[i].name, name) == 0) {
            return kernel_exports[i].address;
        }
    }

    /* Search loaded module exports */
    for (int i = 0; i < KMOD_MAX_MODULES; i++) {
        if (modules[i].state != KMOD_STATE_LIVE) continue;
        for (uint32_t j = 0; j < modules[i].num_exports; j++) {
            if (strcmp(modules[i].exports[j].name, name) == 0) {
                return modules[i].exports[j].address;
            }
        }
    }

    return 0;  /* Not found */
}

/* ============================================================================
 * Module loading
 * ============================================================================ */

/**
 * @brief Find a free module slot
 */
static kmod_t* kmod_alloc_slot(void) {
    for (int i = 0; i < KMOD_MAX_MODULES; i++) {
        if (modules[i].state == KMOD_STATE_UNLOADED) {
            return &modules[i];
        }
    }
    return NULL;
}

/**
 * @brief Extract module name from filename (strip path and extension)
 */
static void kmod_extract_name(const char *filename, char *name, size_t maxlen) {
    /* Find last '/' */
    const char *base = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '/') base = p + 1;
    }

    /* Copy up to '.' or end */
    size_t i = 0;
    for (; base[i] && base[i] != '.' && i < maxlen - 1; i++) {
        name[i] = base[i];
    }
    name[i] = '\0';
}

int kmod_load(const char *filename) {
    if (!kmod_initialized || !filename) return -1;

    /* Check if module is already loaded */
    char modname[KMOD_MAX_NAME];
    kmod_extract_name(filename, modname, KMOD_MAX_NAME);

    if (kmod_find(modname)) {
        printf("KMOD: Module '%s' is already loaded\n", modname);
        return -1;
    }

    /* Allocate a module slot */
    kmod_t *mod = kmod_alloc_slot();
    if (!mod) {
        printf("KMOD: No free module slots\n");
        return -1;
    }

    memset(mod, 0, sizeof(kmod_t));
    strcpy(mod->name, modname);
    mod->state = KMOD_STATE_LOADING;

    serial_printf("kmod: loading module '%s' from '%s'\n", modname, filename);

    /* Read the file from filesystem */
    if (!fat32_is_mounted()) {
        printf("KMOD: No filesystem mounted\n");
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }

    fat32_file_t *file = fat32_open(filename, "r");
    if (!file) {
        printf("KMOD: Cannot open '%s'\n", filename);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }

    uint32_t file_size = file->file_size;
    if (file_size < sizeof(elf64_hdr_t)) {
        printf("KMOD: File too small\n");
        fat32_close(file);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }

    /* Read entire file into memory */
    uint8_t *file_data = (uint8_t *)kalloc(file_size);
    if (!file_data) {
        printf("KMOD: Failed to allocate %d bytes\n", file_size);
        fat32_close(file);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }

    int bytes_read = fat32_read(file, file_data, file_size);
    fat32_close(file);

    if (bytes_read != (int)file_size) {
        printf("KMOD: Short read (%d of %d bytes)\n", bytes_read, file_size);
        kfree(file_data);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }

    /* Validate ELF header */
    elf64_hdr_t *ehdr = (elf64_hdr_t *)file_data;
    if (ehdr->magic != ELF_MAGIC) {
        printf("KMOD: Not an ELF file\n");
        kfree(file_data);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }

    if (ehdr->class != 2 || ehdr->machine != 0x3E) {
        printf("KMOD: Not a 64-bit x86_64 ELF\n");
        kfree(file_data);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }

    if (ehdr->type != 1) {  /* ET_REL */
        printf("KMOD: Not a relocatable object (type=%d)\n", ehdr->type);
        kfree(file_data);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }

    serial_printf("kmod: ELF valid - %d sections, shstrndx=%d\n",
                  ehdr->shnum, ehdr->shstrndx);

    /* Get section headers */
    elf64_shdr_t *shdrs = (elf64_shdr_t *)(file_data + ehdr->shoff);
    uint16_t shnum = ehdr->shnum;

    /* Get section name string table */
    char *shstrtab = NULL;
    if (ehdr->shstrndx < shnum) {
        shstrtab = (char *)(file_data + shdrs[ehdr->shstrndx].offset);
    }

    /* === Phase 1: Calculate total size needed and allocate === */
    uint64_t total_alloc = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        if ((shdrs[i].flags & SHF_ALLOC) && shdrs[i].size > 0) {
            /* Align to 16 bytes */
            total_alloc = (total_alloc + 15) & ~15ULL;
            total_alloc += shdrs[i].size;
        }
    }

    if (total_alloc == 0) {
        printf("KMOD: No loadable sections\n");
        kfree(file_data);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }

    uint8_t *load_base = (uint8_t *)kalloc(total_alloc);
    if (!load_base) {
        printf("KMOD: Failed to allocate %llu bytes for module\n",
               (unsigned long long)total_alloc);
        kfree(file_data);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }
    memset(load_base, 0, total_alloc);

    mod->text_base = load_base;
    mod->text_size = total_alloc;

    /* === Phase 2: Copy sections and record their runtime addresses === */
    uint64_t *section_addrs = (uint64_t *)kalloc(shnum * sizeof(uint64_t));
    if (!section_addrs) {
        kfree(load_base);
        kfree(file_data);
        mod->state = KMOD_STATE_UNLOADED;
        return -1;
    }
    memset(section_addrs, 0, shnum * sizeof(uint64_t));

    uint64_t offset = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        if (!(shdrs[i].flags & SHF_ALLOC) || shdrs[i].size == 0) continue;

        offset = (offset + 15) & ~15ULL;  /* Align */
        section_addrs[i] = (uint64_t)(load_base + offset);

        if (shdrs[i].type == SHT_NOBITS) {
            /* BSS - already zeroed */
        } else {
            memcpy(load_base + offset, file_data + shdrs[i].offset, shdrs[i].size);
        }

        if (shstrtab) {
            serial_printf("kmod:   section '%s' -> 0x%llx (%llu bytes)\n",
                          shstrtab + shdrs[i].name,
                          (unsigned long long)section_addrs[i],
                          (unsigned long long)shdrs[i].size);
        }

        offset += shdrs[i].size;
    }

    /* === Phase 3: Find symbol table and string table === */
    elf64_sym_t *symtab = NULL;
    char *strtab = NULL;
    uint32_t sym_count = 0;

    for (uint16_t i = 0; i < shnum; i++) {
        if (shdrs[i].type == SHT_SYMTAB) {
            symtab = (elf64_sym_t *)(file_data + shdrs[i].offset);
            sym_count = shdrs[i].size / sizeof(elf64_sym_t);
            if (shdrs[i].link < shnum) {
                strtab = (char *)(file_data + shdrs[shdrs[i].link].offset);
            }
            break;
        }
    }

    /* === Phase 4: Process relocations === */
    int unresolved = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        if (shdrs[i].type != SHT_RELA) continue;

        uint32_t target_sec = shdrs[i].info;
        if (target_sec >= shnum || section_addrs[target_sec] == 0) continue;

        elf64_rela_t *relas = (elf64_rela_t *)(file_data + shdrs[i].offset);
        uint32_t rela_count = shdrs[i].size / sizeof(elf64_rela_t);

        for (uint32_t r = 0; r < rela_count; r++) {
            uint32_t sym_idx = ELF64_R_SYM(relas[r].info);
            uint32_t rel_type = ELF64_R_TYPE(relas[r].info);

            if (sym_idx >= sym_count) continue;

            elf64_sym_t *sym = &symtab[sym_idx];
            uint64_t S = 0;  /* Symbol value */

            if (sym->shndx == 0) {
                /* Undefined symbol - resolve from kernel exports */
                const char *sym_name = strtab ? strtab + sym->name : "?";
                S = kmod_resolve_symbol(sym_name);
                if (S == 0) {
                    serial_printf("kmod: unresolved symbol '%s'\n", sym_name);
                    printf("KMOD: Unresolved symbol: %s\n", sym_name);
                    unresolved++;
                }
            } else if (sym->shndx < shnum && section_addrs[sym->shndx]) {
                S = section_addrs[sym->shndx] + sym->value;
            }

            uint64_t P = section_addrs[target_sec] + relas[r].offset;
            int64_t A = relas[r].addend;

            switch (rel_type) {
                case R_X86_64_64:
                    *(uint64_t *)P = S + A;
                    break;
                case R_X86_64_PC32:
                case R_X86_64_PLT32:
                    *(int32_t *)P = (int32_t)(S + A - P);
                    break;
                case R_X86_64_32:
                    *(uint32_t *)P = (uint32_t)(S + A);
                    break;
                case R_X86_64_32S:
                    *(int32_t *)P = (int32_t)(S + A);
                    break;
                default:
                    serial_printf("kmod: unsupported reloc type %d\n", rel_type);
                    break;
            }
        }
    }

    if (unresolved) {
        printf("KMOD: refusing to load '%s' (%d unresolved symbols)\n",
               modname, unresolved);
        kfree(section_addrs);
        kfree(load_base);
        kfree(file_data);
        mod->text_base = NULL;
        mod->state = KMOD_STATE_FAILED;
        return -1;
    }

    /* === Phase 5: Find init/cleanup symbols === */
    if (symtab && strtab) {
        for (uint32_t i = 0; i < sym_count; i++) {
            if (ELF_ST_BIND(symtab[i].info) != STB_GLOBAL) continue;
            if (symtab[i].shndx == 0 || symtab[i].shndx >= shnum) continue;
            if (section_addrs[symtab[i].shndx] == 0) continue;

            const char *sym_name = strtab + symtab[i].name;
            uint64_t sym_addr = section_addrs[symtab[i].shndx] + symtab[i].value;

            if (strcmp(sym_name, "module_init") == 0) {
                mod->init = (int (*)(void))sym_addr;
                serial_printf("kmod: found module_init at 0x%llx\n",
                              (unsigned long long)sym_addr);
            } else if (strcmp(sym_name, "module_cleanup") == 0) {
                mod->cleanup = (void (*)(void))sym_addr;
                serial_printf("kmod: found module_cleanup at 0x%llx\n",
                              (unsigned long long)sym_addr);
            }

            /* Export all global symbols from this module */
            if (mod->num_exports < KMOD_MAX_EXPORTS) {
                strncpy(mod->exports[mod->num_exports].name, sym_name, 63);
                mod->exports[mod->num_exports].name[63] = '\0';
                mod->exports[mod->num_exports].address = sym_addr;
                mod->num_exports++;
            }
        }
    }

    /* Clean up temporary data */
    kfree(section_addrs);
    kfree(file_data);

    /* === Phase 6: Call module init === */
    if (mod->init) {
        serial_printf("kmod: calling module_init() for '%s'\n", modname);
        int ret = mod->init();
        if (ret != 0) {
            printf("KMOD: Module '%s' init failed (return %d)\n", modname, ret);
            kfree(load_base);
            mod->text_base = NULL;
            mod->state = KMOD_STATE_FAILED;
            return -1;
        }
    } else {
        serial_printf("kmod: warning - no module_init() found in '%s'\n", modname);
    }

    mod->state = KMOD_STATE_LIVE;
    printf("KMOD: Module '%s' loaded successfully (%llu bytes, %d exports)\n",
           modname, (unsigned long long)total_alloc, mod->num_exports);

    return 0;
}

/* ============================================================================
 * Module unloading
 * ============================================================================ */

int kmod_unload(const char *name) {
    if (!kmod_initialized || !name) return -1;

    kmod_t *mod = kmod_find(name);
    if (!mod) {
        printf("KMOD: Module '%s' not found\n", name);
        return -1;
    }

    if (mod->state != KMOD_STATE_LIVE) {
        printf("KMOD: Module '%s' is not in LIVE state\n", name);
        return -1;
    }

    if (mod->ref_count > 0) {
        printf("KMOD: Module '%s' is still in use (ref_count=%d)\n",
               name, mod->ref_count);
        return -1;
    }

    mod->state = KMOD_STATE_UNLOADING;

    /* Call cleanup function */
    if (mod->cleanup) {
        serial_printf("kmod: calling module_cleanup() for '%s'\n", name);
        mod->cleanup();
    }

    /* Free memory */
    if (mod->text_base) {
        kfree(mod->text_base);
    }
    if (mod->data_base) {
        kfree(mod->data_base);
    }

    printf("KMOD: Module '%s' unloaded\n", name);

    /* Clear the slot */
    memset(mod, 0, sizeof(kmod_t));
    mod->state = KMOD_STATE_UNLOADED;

    return 0;
}

/* ============================================================================
 * Module queries
 * ============================================================================ */

kmod_t* kmod_find(const char *name) {
    if (!name) return NULL;

    for (int i = 0; i < KMOD_MAX_MODULES; i++) {
        if (modules[i].state != KMOD_STATE_UNLOADED &&
            strcmp(modules[i].name, name) == 0) {
            return &modules[i];
        }
    }
    return NULL;
}

kmod_state_t kmod_get_state(const char *name) {
    kmod_t *mod = kmod_find(name);
    return mod ? mod->state : KMOD_STATE_UNLOADED;
}

uint32_t kmod_count(void) {
    uint32_t count = 0;
    for (int i = 0; i < KMOD_MAX_MODULES; i++) {
        if (modules[i].state == KMOD_STATE_LIVE) count++;
    }
    return count;
}

void kmod_list(void) {
    printf("Loaded kernel modules:\n");
    printf("Name                 Size       Exports    Refs     State\n");
    printf("----                 ----       -------    ----     -----\n");

    bool found = false;
    for (int i = 0; i < KMOD_MAX_MODULES; i++) {
        if (modules[i].state == KMOD_STATE_UNLOADED) continue;
        found = true;

        const char *state_str = "?";
        switch (modules[i].state) {
            case KMOD_STATE_LOADING:   state_str = "loading"; break;
            case KMOD_STATE_LIVE:      state_str = "live"; break;
            case KMOD_STATE_UNLOADING: state_str = "unloading"; break;
            case KMOD_STATE_FAILED:    state_str = "FAILED"; break;
            default: break;
        }

        printf("%s %10llu %10u %8u %s\n",
               modules[i].name,
               (unsigned long long)modules[i].text_size,
               modules[i].num_exports,
               modules[i].ref_count,
               state_str);
    }

    if (!found) {
        printf("  (no modules loaded)\n");
    }
}
