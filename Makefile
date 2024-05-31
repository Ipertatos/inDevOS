# Variables

#use local cross compiler
CC = /home/ipertatos/opt/cross/bin/x86_64-elf-gcc

#CC = gcc
LD = ld
AS = nasm

CFLAGS = -g -m64 -ffreestanding -fno-pie -mgeneral-regs-only -mcmodel=kernel -fcommon
 #-Wall -Wextra
LDFLAGS = -T linker.ld -melf_x86_64
ASFLAGS = -g -F dwarf -f elf64

#c files
C_SOURCES = $(wildcard src/flanterm/*.c) $(wildcard src/flanterm/backends/*.c) $(wildcard src/*.c) 
#assembly files
ASM_SOURCES = $(wildcard src/*.asm) 

files = $(C_SOURCES) $(ASM_SOURCES)
# Object files
OBJ = $(patsubst src/%.c, obj/%.o, $(C_SOURCES)) $(patsubst src/%.asm, obj/%.o, $(ASM_SOURCES))

# Targets
all: os.iso

limine:
	make -C limine

os.iso: boot.bin
	mkdir -p iso/boot/limine
	cp boot.bin iso/boot/
	cp limine.cfg limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso/boot/limine/
	mkdir -p iso/EFI/BOOT
	cp -v limine/BOOTX64.EFI iso/EFI/BOOT/
	cp -v limine/BOOTIA32.EFI iso/EFI/BOOT/
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        iso -o os.iso
		limine/limine bios-install os.iso


boot.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

#compile all asm files
obj/%.o: src/%.asm
	$(AS) $(ASFLAGS) -o $@ $<
#compile all c files
obj/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf *.o *.bin os.iso iso obj/*
	mkdir obj/flanterm
	mkdir obj/flanterm/backends

run: os.iso
	qemu-system-x86_64 os.iso -m 1G

debug: os.iso
	qemu-system-x86_64 -s -S os.iso -d int -M smm=off

.PHONY: all clean