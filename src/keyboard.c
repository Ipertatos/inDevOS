#include "keyboard.h"

bool capslock = false;

int keyboard_init() {
    __asm__ volatile ("cli");
    int i;
    char status;
    for (i = 0; i < 5; i++){
        while (inb(0x0064) & 0x02);
        outb(0x0064, 0xD0);
        while ((inb(0x0064) & 0x01) == 0);
        status = inb(0x0060);
        while (inb(0x0064) & 0x02);
        outb(0x0064, 0xD1);
        while (inb(0x0064) & 0x02);
        status |= 0x02;
        outb(0x0060, status);
        while ((inb(0x0064) & 0x02));
        outb(0x0060, 0xD0);
        while ((inb(0x0064) & 0x01) == 0);
        status = inb(0x0060);
        if(status & 0x02){
            return 0;
        }
        
    } 
    __asm__ volatile ("sti");
    return -1;
}

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);
    if (scancode & 0x80) {
        // Key released
    } else {
        // Key pressed
        switch (scancode) {
            case 0x1E:
                if(capslock)
                    print("a");
                else
                    print("A");
                break;
            case 0x30:
                print("B");
                break;
            case 0x2E:
                print("C");
                break;
            case 0x20:
                print("D");
                break;
            case 0x12:
                print("E");
                break;
            case 0x21:
                print("F");
                break;
            case 0x22:
                print("G");
                break;
            case 0x23:
                print("H");
                break;
            case 0x17:
                print("I");
                break;
            case 0x24:
                print("J");
                break;
            case 0x25:
                print("K");
                break;
            case 0x26:
                print("L");
                break;
            case 0x32:
                print("M");
                break;
            case 0x31:
                print("N");
                break;
            case 0x18:
                print("O");
                break;
            case 0x19:
                print("P");
                break;
            case 0x10:
                print("Q");
                break;
            case 0x13:
                print("R");
                break;
            case 0x1F:
                print("S");
                break;
            case 0x14:
                print("T");
                break;
            case 0x16:
                print("U");
                break;
            case 0x2F:
                print("V");
                break;
            case 0x11:
                print("W");
                break;
            case 0x2D:
                print("X");
                break;
            case 0x15:
                print("Y");
                break;
            case 0x2C:
                print("Z");
                break;
            case 0x02:
                print("1");
                break;
            case 0x03:
                print("2");
                break;
            case 0x04:
                print("3");
                break;
            case 0x05:
                print("4");
                break;
            case 0x06:
                print("5");
                break;
            case 0x07:
                print("6");
                break;
            case 0x08:
                print("7");
                break;
            case 0x09:
                print("8");
                break;
            case 0x0A:
                print("9");
                break;
            case 0x0B:
                print("0");
                break;
            case 0x1C:
                print("\n");
                break;
            case 0x0E:
                print("\b");
                break;
            case 0x39:
                print(" ");
                break;
            case 0x3A:
                capslock = !capslock;
                break;
            default:
                print("?");
                break;
        }
    }
    pic_send_eoi(1);
}