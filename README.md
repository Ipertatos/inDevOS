A simple Operating system made in C and ASM

Currently Working:
APIC PIC BOOT ISRs IRQs Timer(not RTC) Keyboard

TODO:
Commands, Filesystem, Sound, RTC, Memory Managment

Still early development needs time to get to a good working state before the fist polishing steps.
There is a cross-compiler hardcoded in the Makefile change that to normal gcc for it to work with the normal compiler


BUGS:
Currently known bugs is that sometimes we get a random Opcode not found error when booting before the 1st IRQ0 call;
a temp fix is adding an random about of nops before the initTimer function and allow it do not have an issue.
