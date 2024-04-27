#pragma once

#include "types.h"

void print(const char* str);

void printk(const char* str);

void printex(const char* str);

char* int2string(uint32_t n);

char* int2hex(uint32_t value);

uint32_t strlen(const char* str);

bool strEql(const char* str1, const char* str2);
extern void stack_dump();

void dump_hex(char *stack);
void dump_str(char *stack);

void memset(void *dest, char val, uint32_t count);

void call(char* buffer, uint32_t buff_len);