// example.c – writes '!' to VGA text memory and returns
typedef unsigned long long u64;
volatile char *vga = (char *)0xffff8000000b8000ULL;

void _start(void) {
    vga[0] = '!';      // character
    vga[1] = 0x0A;     // light-green on black
    while (1) { asm volatile ("ret"); }  // return immediately
}
