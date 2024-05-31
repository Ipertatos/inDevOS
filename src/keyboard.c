#include "keyboard.h"
#include "apic.h"
#include "acpi.h"
#include "cpu.h"
#include "utils.h"
bool capsOn, capsLock;


char buffer[256];
uint32_t buff_len = 0;
bool tobuffer = true;

const uint32_t UNKNOWN = 0xFFFFFFFF,
               ESC = 0xFFFFFFFF - 1,
               CTRL = 0xFFFFFFFF - 2,
               LSHFT = 0xFFFFFFFF - 3,
               RSHFT = 0xFFFFFFFF - 4,
               ALT = 0xFFFFFFFF - 5,
               F1 = 0xFFFFFFFF - 6,
               F2 = 0xFFFFFFFF - 7,
               F3 = 0xFFFFFFFF - 8,
               F4 = 0xFFFFFFFF - 9,
               F5 = 0xFFFFFFFF - 10,
               F6 = 0xFFFFFFFF - 11,
               F7 = 0xFFFFFFFF - 12,
               F8 = 0xFFFFFFFF - 13,
               F9 = 0xFFFFFFFF - 14,
               F10 = 0xFFFFFFFF - 15,
               F11 = 0xFFFFFFFF - 16,
               F12 = 0xFFFFFFFF - 17,
               SCRLCK = 0xFFFFFFFF - 18,
               HOME = 0xFFFFFFFF - 19,
               UP = 0xFFFFFFFF - 20,
               LEFT = 0xFFFFFFFF - 21,
               RIGHT = 0xFFFFFFFF - 22,
               DOWN = 0xFFFFFFFF - 23,
               PGUP = 0xFFFFFFFF - 24,
               PGDOWN = 0xFFFFFFFF - 25,
               END = 0xFFFFFFFF - 26,
               INS = 0xFFFFFFFF - 27,
               DEL = 0xFFFFFFFF - 28,
               CAPS = 0xFFFFFFFF - 29,
               NONE = 0xFFFFFFFF - 30,
               ALTGR = 0xFFFFFFFF - 31,
               NUMLCK = 0xFFFFFFFF - 32;

const uint32_t lowercase[128] = {
UNKNOWN,ESC,'1','2','3','4','5','6','7','8',
'9','0','-','=','\b','\t','q','w','e','r',
't','y','u','i','o','p','[',']','\n',CTRL,
'a','s','d','f','g','h','j','k','l',';',
'\'','`',LSHFT,'\\','z','x','c','v','b','n','m',',',
'.','/',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',LEFT,UNKNOWN,RIGHT,
'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,UNKNOWN,F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

const uint32_t uppercase[128] = {
    UNKNOWN,ESC,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t','Q','W','E','R',
'T','Y','U','I','O','P','{','}','\n',CTRL,'A','S','D','F','G','H','J','K','L',':','"','~',LSHFT,'|','Z','X','C',
'V','B','N','M','<','>','?',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',
LEFT,UNKNOWN,RIGHT,'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,UNKNOWN,F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

void keyboard_init(){
    capsOn = false;
    capsLock = false;
    //init buffer
    memset(&buffer,'\0',sizeof(char)*256);

    while ((inb(0x64) & 2) != 0)
    {
        outb(0x64, 0xAD);
    }
    while ((inb(0x64) & 2) != 0)
    {
        outb(0x64, 0xA7);
    }
    inb(0x60);
    while ((inb(0x64) & 2) != 0)
    {
        outb(0x64, 0x20);
    }
    uint8_t config = inb(0x60);
    config |= (1 << 0) | (1 << 6);
    if(config & (1 << 5) != 0){
        config |= (1 << 1);
    }
    while ((inb(0x64) & 2) != 0)
    {
        outb(0x64, 0x60);
    }
    while ((inb(0x64) & 2) != 0)
    {
        outb(0x60, config);
    }
    ioapic_redirect_irq(bsp_lapic_id, 33, 1, true);
    while ((inb(0x64) & 2) != 0)
    {
        outb(0x64, 0xAE);
    }
    while ((inb(0x64) & 2) != 0)
    {
        outb(0x64, 0x20);
    }
    if(config & (1 << 5) != 0){
        while ((inb(0x64) & 2) != 0)
        {
            outb(0x64, 0xA8);
        }
    }
    
}

char ps2_translate2ascii(uint16_t scancode){
    if(scancode > 0x58)
        return 0;
    
    if(scancode == 0x2A || scancode == 0x36){
        capsOn = !capsOn;
        return 0;
    }

    if(scancode == 0x3A){
        capsLock = !capsLock;
        return 0;
    }

    if(lowercase[scancode] == UNKNOWN)
        return 0;
    
    if(capsOn && !capsLock)
        return uppercase[scancode];
    else if(capsLock && !capsOn)
        return uppercase[scancode];
    else
        return lowercase[scancode];

}

void keyboard_handler(){
    uint16_t scancode = inb(0x60);
    
    char ch = ps2_translate2ascii(scancode);
    if(ch == 0){
        apic_eoi();
        return;
    }
    //add to buffer
    if(tobuffer){
        buffer[buff_len] = ch;
        buff_len++;
    }
    else{
        //send to terminal
        printf("{c}",ch);
    }
    apic_eoi();
}