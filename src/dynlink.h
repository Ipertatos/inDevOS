/**
 * @file dynlink.h
 * @brief Dynamic linker / shared library loader
 *
 * Provides runtime dynamic linking for ELF shared objects:
 *   - Load .so files from FAT32 into process address space
 *   - Resolve symbols across loaded libraries
 *   - Apply relocations (R_X86_64_RELATIVE, R_X86_64_64, R_X86_64_GLOB_DAT,
 *     R_X86_64_JUMP_SLOT, R_X86_64_COPY)
 *   - dlopen / dlsym / dlclose API for user programs
 *   - Lazy / eager binding support
 *   - Simple symbol table lookup
 */
#ifndef DYNLINK_H
#define DYNLINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------- limits ---------- */
#define DYNLINK_MAX_LIBS        16      /* max loaded shared objects */
#define DYNLINK_MAX_SYMBOLS     512     /* total symbols across all libs */
#define DYNLINK_MAX_NAME_LEN    128     /* max symbol / lib name */

/* ---------- ELF dynamic tags we care about ---------- */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA_TAG     7       /* renamed to avoid collision with exec.c */
#define DT_RELASZ_TAG   8
#define DT_RELAENT_TAG  9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_SONAME       14
#define DT_RPATH        15
#define DT_SYMBOLIC     16
#define DT_JMPREL       23
#define DT_PLTREL       20

/* ---------- ELF symbol binding / type ---------- */
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2

/* ---------- ELF relocation types (x86-64) ---------- */
#define R_X86_64_NONE       0
#define R_X86_64_64_TYPE    1
#define R_X86_64_GLOB_DAT_TYPE  6
#define R_X86_64_JUMP_SLOT_TYPE 7
#define R_X86_64_RELATIVE_TYPE  8
#define R_X86_64_COPY_TYPE  5

/* ---------- ELF structures for .so parsing ---------- */

typedef struct __attribute__((packed)) {
    uint32_t st_name;       /* string table index */
    uint8_t  st_info;       /* type and binding */
    uint8_t  st_other;
    uint16_t st_shndx;      /* section header index */
    uint64_t st_value;      /* symbol value */
    uint64_t st_size;       /* symbol size */
} dynlink_elf_sym_t;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} dynlink_elf_rela_t;

#define ELF64_R_SYM(i)     ((i) >> 32)
#define ELF64_R_TYPE(i)    ((i) & 0xffffffffL)
#define ELF64_ST_BIND(i)   ((i) >> 4)
#define ELF64_ST_TYPE(i)   ((i) & 0xf)

/* ---------- loaded library descriptor ---------- */

typedef struct {
    char     name[DYNLINK_MAX_NAME_LEN];
    uint64_t load_base;         /* base virtual address in process space */
    uint64_t load_size;         /* total mapped size */
    void    *image;             /* raw file image (kalloc'd) */
    uint32_t image_size;
    int      ref_count;

    /* Dynamic section pointers (relative to load_base) */
    const char           *strtab;
    const dynlink_elf_sym_t *symtab;
    uint32_t              sym_count;     /* number of symbols */
    const dynlink_elf_rela_t *rela;
    uint32_t              rela_count;
    const dynlink_elf_rela_t *pltrel;
    uint32_t              pltrel_count;

    uint64_t init_func;         /* DT_INIT address */
    uint64_t fini_func;         /* DT_FINI address */

    bool     loaded;
} dynlink_lib_t;

/* ---------- global symbol entry ---------- */

typedef struct {
    char     name[DYNLINK_MAX_NAME_LEN];
    uint64_t address;           /* resolved virtual address */
    int      lib_index;         /* which library it belongs to */
    uint8_t  binding;           /* STB_GLOBAL, STB_WEAK, etc. */
    uint8_t  type;              /* STT_FUNC, STT_OBJECT, etc. */
} dynlink_symbol_t;

/* ---------- public API ---------- */

/**
 * Initialise the dynamic linker subsystem.
 */
void dynlink_init(void);

/**
 * Load a shared library (.so) from FAT32 into memory.
 * Returns a handle (library index) or -1 on failure.
 */
int dynlink_load(const char *path);

/**
 * Look up a symbol by name across all loaded libraries.
 * Returns the symbol's virtual address, or 0 if not found.
 */
uint64_t dynlink_resolve(const char *name);

/**
 * Unload a shared library (decrements ref count; frees if 0).
 */
int dynlink_unload(int handle);

/**
 * List all loaded libraries and their symbols (debug).
 */
void dynlink_list(void);

/* ---------- dlopen/dlsym/dlclose (user-facing wrappers) ---------- */

/**
 * Open a shared library.
 * @param path  Path to the .so file
 * @param flags Reserved (use 0)
 * @return Opaque handle or NULL on failure
 */
void *dl_open(const char *path, int flags);

/**
 * Look up a symbol in a loaded library.
 * @param handle  Handle from dl_open (NULL = search all)
 * @param name    Symbol name
 * @return Pointer to the symbol, or NULL
 */
void *dl_sym(void *handle, const char *name);

/**
 * Close a shared library.
 */
int dl_close(void *handle);

/**
 * Register a built-in kernel symbol so that shared libraries can call
 * kernel functions (e.g. printf, malloc, etc.).
 */
int dynlink_register_symbol(const char *name, uint64_t address);

#endif /* DYNLINK_H */
