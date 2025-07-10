# Variables

#use local cross compiler
ifeq ($(wildcard ~/opt/cross/bin/x86_64-elf-gcc),~/opt/cross/bin/x86_64-elf-gcc)
    CC = ~/opt/cross/bin/x86_64-elf-gcc
else
    CC = gcc
endif
#CC = gcc
LD = ld
AS = nasm

BASE_CFLAGS = -g -m64 -ffreestanding -fno-pie -mgeneral-regs-only -mcmodel=kernel -fcommon -DDEBUG_PRINT
CFLAGS = $(BASE_CFLAGS) -Iflanterm -Iflanterm/backends
DEBUG_CFLAGS = $(BASE_CFLAGS) -DDEBUG
 #-Wall -Wextra
LDFLAGS = -T linker.ld -melf_x86_64
ASFLAGS = -g -F dwarf -f elf64

#c files in all directories and subdirectories
C_SOURCES = $(shell find src -name '*.c' ! -name 'kernel_symtab.c')
#C_SOURCES = $(wildcard src/flanterm/*.c) $(wildcard src/flanterm/backends/*.c) $(wildcard src/*.c) 
#assembly files
ASM_SOURCES = $(shell find src -name '*.asm')
#ASM_SOURCES = $(wildcard src/*.asm) 

files = $(C_SOURCES) $(ASM_SOURCES)
# Object files (excluding kernel_symtab.o)
OBJ = $(patsubst src/%.c, obj/%.o, $(C_SOURCES)) $(patsubst src/%.asm, obj/%.o, $(ASM_SOURCES))

# Targets
all: kernel_final.bin

limine:
	make -C limine

# First phase: build kernel without symbols
boot.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

# Generate symbol table from first phase kernel
kernel.sym: boot.bin
	nm -n boot.bin | grep ' [tT] ' > kernel.sym

# Generate symbol table source
src/kernel_symtab.c: kernel.sym symtab_gen.py
	python3 symtab_gen.py < kernel.sym > src/kernel_symtab.c

# Build symbol table object
obj/kernel_symtab.o: src/kernel_symtab.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Second phase: build final kernel with symbols
kernel_final.bin: $(OBJ) obj/kernel_symtab.o
	$(LD) $(LDFLAGS) -o $@ $^

# Update ISO generation to use final kernel
os.iso: kernel_final.bin
	mkdir -p iso/boot/limine
	cp kernel_final.bin iso/boot/boot.bin
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

#compile all asm files
obj/%.o: src/%.asm
	mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -o $@ $<
#compile all c files
obj/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# flanterm sources no longer used

clean:
	rm -rf *.o *.bin os.iso iso obj/*
	rm -rf kernel.sym src/kernel_symtab.c kernel_final.bin
	mkdir obj/flantern
	mkdir obj/flanterm/backends

run: os.iso
	qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -net nic,model=rtl8139 -drive file=sata_disk.img,format=raw,index=0 -drive file=nvme_disk.img,format=raw,index=1,media=disk -serial file:serial.log

debug: os.iso
	qemu-system-x86_64 -s -S -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -net nic,model=rtl8139 -drive file=sata_disk.img,format=raw,index=0 -drive file=nvme_disk.img,format=raw,index=1,media=disk -serial file:serial.log

debug-detailed: os.iso
	qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log -d int,mmu,guest_errors,cpu_reset -D qemu-debug.log

debug-gdb: os.iso
	qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log -s -S

# Debug build with function entry logging
debug-build: clean
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" os.iso

debug-run: debug-build
	qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log

.PHONY: all clean debug-build debug-run
