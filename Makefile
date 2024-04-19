# Variables
CC = gcc
LD = ld
AS = nasm

CFLAGS = -g -m64 -ffreestanding -fno-pie
 #-Wall -Wextra
LDFLAGS = -T linker.ld -melf_x86_64
ASFLAGS = -g -F dwarf -f elf64

#c files
C_SOURCES = $(wildcard src/*.c)
#assembly files
ASM_SOURCES = $(wildcard src/*.asm)

files = $(C_SOURCES) $(ASM_SOURCES)
# Object files
OBJ = $(patsubst src/%.c, obj/%.o, $(C_SOURCES)) $(patsubst src/%.asm, obj/%.o, $(ASM_SOURCES))

# Targets
all: os.iso

os.iso: boot.bin
	mkdir -p iso/boot/grub
	cp boot.bin iso/boot/
	cp grub.cfg iso/boot/grub/
	grub-mkrescue -o os.iso iso


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

run: os.iso
	qemu-system-x86_64 os.iso

debug: os.iso
	qemu-system-x86_64 -s -S os.iso

.PHONY: all clean