#pragma once

#include "types.h"
#include "registers.h"

typedef struct __attribute__((packed)) {
    uint16_t size;
    uint64_t offset;
} gdt_pointer_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} gdt_entry_t;  


typedef struct {
    uint32_t reseved1;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved2;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved3;
    uint16_t reserved4;
    uint16_t iopb;
} __attribute__((packed)) tss_t;

void gdt_init(void);
void gdt_set_entry(int num, uint64_t base, uint64_t limit,uint8_t access, uint8_t granularity);

extern void s_setgdt(uint32_t limit, uint64_t base);
extern void s_flushgdt(void);
extern void s_settss(void);