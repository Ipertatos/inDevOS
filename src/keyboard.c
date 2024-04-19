#include "keyboard.h"

bool capsOn, capsLock;

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
    irq_install_handler(1, &keyboard_handler);
}

void keyboard_handler(registers_t *regs)
{
    char scancode = inb(0x60)&0x7F; //read the scan code
    char press = inb(0x60)&0x80; //read the press/release bit

    switch (scancode)
    {
    case 1:
    case 29:
    case 56:
    case 59:
    case 61:
    case 62:
    case 63:
    case 64:
    case 65:
    case 66:
    case 67:
    case 68:
    case 87:
    case 88:
        break;
    case 42:
        //shift 
        if(press==0)
            capsOn = true;
        else
            capsOn = false;

        break;
    case 58:
        //caps lock
        if(!capsLock && press==0)
            capsLock = true;
        else if(capsLock && press==0)
            capsLock = false;
        
        break;
    default:
        if(press==0)
            if(capsOn || capsLock){
                print(uppercase[scancode]);
            }else{
                print(lowercase[scancode]);
            }
    }
}