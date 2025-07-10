#pragma once
#include <stddef.h>
#include <stdint.h>

void vga_init(void);
void vga_putchar(char c);
void vga_write(const char *buf, size_t len);
void vga_clear(void);
uint16_t *vga_get_buffer(void); 