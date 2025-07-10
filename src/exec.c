#include "exec.h"
#include "fat32.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"
#include "debug.h"
#include "term.h" // for printf

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
    
    uint8_t *bytes = (uint8_t *)mem;
    for (uint32_t i = 0; i < size; i += 16) {
        printf("0x%llx: ", (unsigned long long)(load_addr + i));
        
        // Print hex bytes
        for (uint32_t j = 0; j < 16 && (i + j) < size; j++) {
            printf("%02x ", bytes[i + j]);
        }
        
        // Pad if less than 16 bytes
        for (uint32_t j = (size - i < 16) ? size - i : 16; j < 16; j++) {
            printf("   ");
        }
        
        printf(" | ");
        
        // Print ASCII representation
        for (uint32_t j = 0; j < 16 && (i + j) < size; j++) {
            uint8_t c = bytes[i + j];
            printf("%c", (c >= 32 && c <= 126) ? c : '.');
        }
        
        printf("\n");
    }
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

int exec_file(const char *filename) {
    if (!fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return -1;
    }

    fat32_file_t *file = fat32_open(filename, "r");
    if (!file) {
        printf("exec: failed to open '%s'\n", filename);
        return -1;
    }

    /* Allocate buffer and read the entire file */
    uint32_t fsize = file->file_size;
    if (fsize == 0) {
        printf("exec: file is empty\n");
        fat32_close(file);
        return -1;
    }

    void *buffer = kalloc(fsize);
    if (!buffer) {
        printf("exec: out of memory allocating %u bytes\n", fsize);
        fat32_close(file);
        return -1;
    }

    int bytes_read = fat32_read(file, buffer, fsize);
    fat32_close(file);
    if (bytes_read != (int)fsize) {
        printf("exec: failed to read file, expected %u got %d\n", fsize, bytes_read);
        kfree(buffer);
        return -1;
    }

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

    /* Call the entry point */
    entry();

    printf("exec: program '%s' returned\n", name);

    kfree(image);
    return 0;
}

/* Execute raw binary assuming entry at image base */
static int exec_run_raw(void *image, uint32_t size, const char *name) {
    (void)size; // Unused currently
    printf("exec: executing raw binary '%s' (entry at base)\n", name);
    debug_dump_executable(image, size, (uint64_t)image);
    fix_flat_binary_relocations(image, size, (uint64_t)image);
    void (*entry)(void) = (void (*)(void))image;
    entry();
    printf("exec: program '%s' returned\n", name);
    kfree(image);
    return 0;
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

static int exec_run_elf(void *image, uint32_t size, const char *name) {
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

    /* Find range of loadable segments */
    uint64_t base = UINT64_MAX;
    uint64_t end  = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_filesz == 0 && ph->p_memsz == 0) continue;
        if (ph->p_vaddr < base) base = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > end) end = ph->p_vaddr + ph->p_memsz;
    }

    if (base == UINT64_MAX) {
        printf("exec: no loadable segments\n");
        kfree(image);
        return -1;
    }

    uint64_t total_size = end - base;
    void *mem = kalloc((size_t)total_size);
    if (!mem) {
        printf("exec: cannot allocate %llu bytes for ELF\n", (unsigned long long)total_size);
        kfree(image);
        return -1;
    }
    /* zero BSS area */
    memset(mem, 0, total_size);

    /* Copy segments */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_filesz == 0) continue;
        uint64_t dest_off = ph->p_vaddr - base;
        if (dest_off + ph->p_filesz > total_size) {
            printf("exec: segment outside allocated memory\n");
            kfree(mem);
            kfree(image);
            return -1;
        }
        memmove((uint8_t *)mem + dest_off, (uint8_t *)image + ph->p_offset, ph->p_filesz);
    }

    Elf64_Phdr *dyn_ph = NULL;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_DYNAMIC) {
            dyn_ph = ph;
            break;
        }
    }

    if (dyn_ph) {
        // Dynamic section is in the loaded memory at (mem + (dyn_ph->p_vaddr - base))
        uint8_t *dyn_base = (uint8_t *)mem + (dyn_ph->p_vaddr - base);
        uint64_t rela = 0, relasz = 0, relaent = 0;

        // Parse dynamic entries
        for (Elf64_Dyn *dyn = (Elf64_Dyn *)dyn_base; dyn->d_tag != 0; dyn++) {
            if (dyn->d_tag == DT_RELA) rela = dyn->d_val;
            else if (dyn->d_tag == DT_RELASZ) relasz = dyn->d_val;
            else if (dyn->d_tag == DT_RELAENT) relaent = dyn->d_val;
        }

        if (rela && relasz && relaent) {
            uint64_t load_base = (uint64_t)mem - base;  // Actual load address adjustment
            uint8_t *rela_table = (uint8_t *)mem + (rela - base);

            for (uint64_t i = 0; i < relasz / relaent; i++) {
                Elf64_Rela *r = (Elf64_Rela *)(rela_table + i * relaent);
                if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE) {
                    uint64_t *target = (uint64_t *)((uint8_t *)mem + (r->r_offset - base));
                    *target = load_base + r->r_addend;
                } else if (ELF64_R_TYPE(r->r_info) == R_X86_64_64) {
                    uint64_t *target = (uint64_t *)((uint8_t *)mem + (r->r_offset - base));
                    *target = load_base + r->r_addend;  // For static binaries, symbol value is in addend
                } else if (ELF64_R_TYPE(r->r_info) == R_X86_64_GLOB_DAT) {
                    uint64_t *target = (uint64_t *)((uint8_t *)mem + (r->r_offset - base));
                    *target = load_base + r->r_addend;  // Similar to R_X86_64_64 for static
                } else {
                    printf("exec: unsupported relocation type %llu\n", (unsigned long long)ELF64_R_TYPE(r->r_info));
                    printf("exec: continuing anyway...\n");
                }
            }
            printf("exec: applied %llu relocations\n", (unsigned long long)(relasz / relaent));
        }
    }

    uint64_t entry_off = eh->e_entry - base;
    printf("exec: executing ELF '%s' entry offset 0x%llx size %llu bytes.\n", name, (unsigned long long)entry_off, (unsigned long long)total_size);
    debug_dump_executable(mem, total_size, (uint64_t)mem);

    void (*entry)(void) = (void (*)(void))((uint8_t *)mem + entry_off);
    entry();

    printf("exec: program '%s' returned\n", name);

    kfree(mem);
    kfree(image);
    return 0;
} 