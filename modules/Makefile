# Build relocatable ELF kernel modules (.ko) for inDevOS kmod loader.

CROSS_GCC := $(wildcard $(HOME)/opt/cross/bin/x86_64-elf-gcc)
ifneq ($(CROSS_GCC),)
    CC ?= $(CROSS_GCC)
else
    CC ?= gcc
endif

MOD_CFLAGS = -ffreestanding -fno-pie -fno-pic -fno-plt -fno-stack-protector \
             -m64 -mcmodel=large -mno-red-zone -mgeneral-regs-only \
             -O2 -Wall -Wextra -Wno-unused-parameter

MODULES = test_driver.ko

.PHONY: all clean

all: $(MODULES)

test_driver.ko: test_driver.o
	$(CC) -r -nostdlib -o $@ $^

test_driver.o: test_driver.c
	$(CC) $(MOD_CFLAGS) -c -o $@ $<

clean:
	rm -f *.o *.ko
