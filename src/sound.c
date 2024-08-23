#include "sound.h"
#include "hpet.h"
#include "utils.h"
wav_header wav;

static void play_sound(uint32_t nFrequence) {
    uint32_t Div;
    uint8_t tmp;

    // Set the PIT to the desired frequency
    if(nFrequence == 0) {
        nFrequence = 1;
    }
    Div = 1193180 / nFrequence;
    outb(0x43, 0xb6);
    outb(0x42, (uint8_t) (Div) );
    outb(0x42, (uint8_t) (Div >> 8));

    // And play the sound using the PC speaker
    tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }
}

static void nosound() {
    uint8_t tmp = inb(0x61) & 0xFC;

    outb(0x61, tmp);
}

void beep() {
    play_sound(1000);
    hpet_sleep_counter(100);
    nosound();
    hpet_sleep_counter(10);
    printf("stopped beeping{n}");
}

void test_sound() {
    for (uint32_t i = 1; i < 2147483647; i++)
    {
        play_sound(i);
        hpet_sleep_counter(10);
        nosound();
    }
    
}
void PlayWAV(uint32_t sample_rate, uint32_t file_size, uint8_t* file_data) {
    
    uint32_t Div;
    uint8_t tmp;

    // Set the PIT to the desired frequency
    Div = 1193180 / sample_rate;
    outb(0x43, 0x36);
    outb(0x42, (uint8_t) (Div) );
    outb(0x42, (uint8_t) (Div >> 8));

    uint32_t i;
    tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }

    for (i = 0; i < file_size; i++) {
            // And play the sound using the PC speaker
        outb(0x42,file_data[i]);
        __asm__ __volatile__("hlt");
    }
    printf("stopped playing sound{n}");
    // Disable digitized sound
    nosound();
}