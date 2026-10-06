# Variables
ENABLE_LUA ?= 1

# Prefer a freestanding cross compiler when installed (empty CC from env is ok)
CROSS_GCC := $(wildcard $(HOME)/opt/cross/bin/x86_64-elf-gcc)
ifneq ($(CROSS_GCC),)
    CC ?= $(CROSS_GCC)
else
    CC ?= gcc
endif
#CC = gcc
LD = ld
AS = nasm

# Enable warnings to catch common issues
# Note: Some warnings are suppressed for legacy code compatibility
WARN_FLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
             -Wno-unused-variable -Wno-sign-compare \
             -Werror=implicit-function-declaration -Werror=return-type

# -O2: optimize; -fno-strict-aliasing: kernel code does type punning;
# -fno-omit-frame-pointer: keep frame pointers for stack traces (debug.c)
# -U_FORTIFY_SOURCE: Ubuntu gcc force-enables fortify at -O1+, which emits
#   calls to host libc __*_chk symbols that don't exist in a freestanding kernel
# Stack protector: the default guard is %fs:0x28 (TLS), which the kernel does
#   not set up, so use the global __stack_chk_guard seeded in kmain().
STACK_PROTECTOR ?= 1
ifeq ($(STACK_PROTECTOR),1)
    SSP_FLAGS = -fstack-protector-strong -mstack-protector-guard=global
else
    SSP_FLAGS = -fno-stack-protector
endif
OPT_FLAGS = -O2 -fno-strict-aliasing -fno-omit-frame-pointer \
            $(SSP_FLAGS) -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0

# -MMD -MP: emit .d dependency files so header edits trigger rebuilds
DEP_FLAGS = -MMD -MP

BASE_CFLAGS = -g $(OPT_FLAGS) $(DEP_FLAGS) -m64 -ffreestanding -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -fcommon $(WARN_FLAGS)
# Enable Lua by defining ENABLE_LUA and adding include path and sources.
# Example (uncomment to enable if you have vendored Lua under third_party/lua-5.4):
# CFLAGS += -DENABLE_LUA -Ithird_party/lua-5.4
# LUA_SRC = $(wildcard third_party/lua-5.4/src/*.c)
# C_SOURCES += $(LUA_SRC)

# NOTE: no global -msse2 here! -mgeneral-regs-only must win for general kernel
# code: at -O2 GCC vectorizes memset/memcpy with SSE, which #UDs before
# fpu_init() enables SSE. Files that genuinely need doubles/SSE get -msse2 via
# the per-object overrides below (term.o, libc_shim.o, sdl_*.o, Lua).
# -Ilibc: minimal freestanding C library headers.  The kernel links no host
#   libc, so <string.h>/<math.h>/<setjmp.h>/... resolve to the in-kernel
#   implementations (mem.c, string.c, libc_shim.c, asmutils.asm) instead of
#   whichever host libc headers happen to be installed.  See src/libc/README.md.
CFLAGS = $(BASE_CFLAGS) -Ilimine -Isrc/libc
DEBUG_CFLAGS = $(BASE_CFLAGS) -DDEBUG
 #-Wall -Wextra
LDFLAGS = -T linker.ld -melf_x86_64 -z noexecstack
ASFLAGS = -g -F dwarf -f elf64

# Optional audio flags (disabled by default; WSL lacks ALSA)
AUDIO ?= 0
ifeq ($(AUDIO),1)
    AUDIO_FLAGS = -audiodev alsa,id=speaker -machine pcspk-audiodev=speaker
else
    AUDIO_FLAGS =
endif

# QEMU display: virtio-vga exposes VGA-compatible boot FB (Limine menu works) AND
# the virtio-gpu protocol our kernel driver uses.  virtio-gpu-pci alone has no VGA —
# the QEMU window stays blank until guest code drives scanout (Limine never loads).
# Match limine.cfg: INTERFACE_RESOLUTION + entry RESOLUTION.
# Override: make run VIRTIO_GPU_X=1920 VIRTIO_GPU_Y=1080
VIRTIO_GPU_X ?= 1920
VIRTIO_GPU_Y ?= 1080
VIRTIO_GPU_DISPLAY = virtio-vga,xres=$(VIRTIO_GPU_X),yres=$(VIRTIO_GPU_Y)
VIRTIO_GPU_PCI     = virtio-gpu-pci,xres=$(VIRTIO_GPU_X),yres=$(VIRTIO_GPU_Y)
VIRTIO_VGA_DEV     = $(VIRTIO_GPU_DISPLAY)

#c files in all directories and subdirectories
C_SOURCES = $(shell find src -name '*.c' ! -name 'kernel_symtab.c')
# Exclude Lua-related files when ENABLE_LUA=0
ifeq ($(ENABLE_LUA),0)
    C_SOURCES := $(filter-out src/luade.c src/luade_graphics.c src/luade_input.c src/lua_runtime.c, $(C_SOURCES))
endif
# In-kernel SDL shim (SYS_SDL_* syscalls for user apps) — not host SDL.
SDL_SOURCES = src/sdl_main.c src/sdl_video.c src/sdl_renderer.c src/sdl_events.c src/sdl_timer.c src/sdl_integration.c src/sdl_kernel_log.c
ASM_SOURCES = $(shell find src -name '*.asm')

# Object files (excluding kernel_symtab.o)
OBJ = $(patsubst src/%.c, obj/%.o, $(C_SOURCES)) $(patsubst src/%.asm, obj/%.o, $(ASM_SOURCES))
SDL_OBJ = $(patsubst src/%.c, obj/%.o, $(SDL_SOURCES))

# Targets
all: lua kernel_final.bin

.PHONY: modules test-driver
modules test-driver:
	$(MAKE) -C modules

# Copy test module onto the SATA disk image (requires sudo + mounted loop)
install-test-driver: modules/test_driver.ko
	@if [[ -f sata_disk.img ]]; then \
		bash ./putfilesinsata.sh modules/test_driver.ko; \
	else \
		echo "sata_disk.img not found — run create_disk_images.sh first"; \
		exit 1; \
	fi

# Always enable Lua; auto-fetch vendor sources if missing
# Vendor lives under third_party as produced by scripts/fetch_lua.sh
#
# Resolve the vendored tree by looking for the sources rather than trusting the
# unversioned "lua-5.4" name: on some checkouts that is a symlink/junction to
# the real lua-5.4.x directory, and NTFS junctions are not always usable (the
# Windows build would then fail to find any Lua source).
LUA_DIR := $(patsubst %/src/lapi.c,%,$(firstword $(wildcard third_party/lua-5.4*/src/lapi.c)))
ifeq ($(strip $(LUA_DIR)),)
LUA_DIR := third_party/lua-5.4
endif
LUA_MARKER := $(LUA_DIR)/src/lapi.c
ifeq ($(ENABLE_LUA),1)
# Note: luaconf.h unconditionally defines LUA_FLOAT_TYPE (defaults to double),
# so no -D override is needed (it would just be redefined anyway).
CFLAGS += -DENABLE_LUA -I$(LUA_DIR)/src
endif
# Lua core plus lauxlib and selected standard libraries (string, table, math).
ifeq ($(ENABLE_LUA),1)
LUA_CORE_SRC = \
    $(LUA_DIR)/src/lapi.c \
    $(LUA_DIR)/src/lcode.c \
    $(LUA_DIR)/src/lctype.c \
    $(LUA_DIR)/src/ldebug.c \
    $(LUA_DIR)/src/ldo.c \
    $(LUA_DIR)/src/ldump.c \
    $(LUA_DIR)/src/lfunc.c \
    $(LUA_DIR)/src/lgc.c \
    $(LUA_DIR)/src/llex.c \
    $(LUA_DIR)/src/lmem.c \
    $(LUA_DIR)/src/lobject.c \
    $(LUA_DIR)/src/lopcodes.c \
    $(LUA_DIR)/src/lparser.c \
    $(LUA_DIR)/src/lstate.c \
    $(LUA_DIR)/src/lstring.c \
    $(LUA_DIR)/src/ltable.c \
    $(LUA_DIR)/src/ltm.c \
    $(LUA_DIR)/src/lundump.c \
    $(LUA_DIR)/src/lvm.c \
    $(LUA_DIR)/src/lzio.c \
    $(LUA_DIR)/src/lauxlib.c \
    $(LUA_DIR)/src/lstrlib.c \
    $(LUA_DIR)/src/ltablib.c \
    $(LUA_DIR)/src/lmathlib.c
LUA_OBJS = $(patsubst $(LUA_DIR)/src/%.c, obj/lua/%.o, $(LUA_CORE_SRC))
OBJ += $(LUA_OBJS)
endif

# Non-Lua objects
OBJ_NONLUA = $(OBJ)

# Auto-fetch Lua sources if missing
$(LUA_MARKER):
	@echo "Fetching Lua vendor sources..."
	bash scripts/fetch_lua.sh

# Ensure Lua vendor is present before compiling Lua objects
$(LUA_OBJS): $(LUA_MARKER)

# Explicit aggregation target for Lua objects to avoid race on first build
.PHONY: lua
lua: $(LUA_MARKER) $(LUA_OBJS)
	@echo "Lua compilation complete"

# Ensure all non-Lua objects wait for lua objects to be built first
$(OBJ_NONLUA): | lua

# Ensure vendor headers exist before compiling any sources that include <lua.h>
$(OBJ): | $(LUA_MARKER)


limine:
	make -C limine

# os_full.iso: bootable ISO with /install/ for in-OS installer (kernel, limine files)
# Use this when booting to run the installer. os.iso stays minimal for normal use.
os_full.iso: kernel_final.bin | lua
	mkdir -p iso_full/boot/limine iso_full/EFI/BOOT iso_full/install
	cp kernel_final.bin iso_full/boot/boot.bin
	cp limine.cfg limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_full/boot/limine/
	cp -v limine/BOOTX64.EFI limine/BOOTIA32.EFI iso_full/EFI/BOOT/
	cp kernel_final.bin iso_full/install/boot.bin
	cp limine.cfg iso_full/install/limine.cfg
	cp limine/limine-bios.sys iso_full/install/limine-bios.sys
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        iso_full -o os_full.iso
	limine/limine bios-install os_full.iso

# Prepare install media: os_full.iso (for CD boot with /install/ on disc)
# Also run ./setup_install_media.sh to prepare sata_disk.img for disk-based install path
install-media: os_full.iso
	@echo ">>> os_full.iso ready. Run ./setup_install_media.sh (requires sudo) to prepare sata_disk.img for install."

# os_install.iso: Limine (same packing as os.iso) plus a FAT32 root partition
# with the real apps and Doom. The kernel mounts that partition as /.
# Windows: .\make_install_iso.ps1
install-iso:
	bash ./make_install_iso.sh


# First phase: build kernel without symbols
boot.bin: $(OBJ_NONLUA) $(SDL_OBJ) $(if $(filter 1,$(ENABLE_LUA)),$(LUA_OBJS))
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
kernel_final.bin: $(OBJ) $(SDL_OBJ) obj/kernel_symtab.o | lua
	$(LD) $(LDFLAGS) -o $@ $^

# Update ISO generation to use final kernel.
# limine.cfg is a prerequisite so that editing the boot menu config actually
# rebuilds the ISO (without it, make considered os.iso up to date and the
# change never reached the image).
os.iso: kernel_final.bin limine.cfg | lua
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

# Files that use double / SSE ABI (printf, Lua, SDL shim)
obj/term.o obj/libc_shim.o: CFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse2
obj/sdl_%.o: CFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse2

# compile vendored lua sources
obj/lua/%.o: $(LUA_DIR)/src/%.c
	mkdir -p $(dir $@)
	$(CC) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse2 -c -o $@ $<

# flanterm sources no longer used (vendored submodule still at ./flanterm for
# historical headers / reference; Makefile.windows -Iflanterm is leftover-safe)

# Auto-generated header dependency files
-include $(OBJ:.o=.d) $(SDL_OBJ:.o=.d) obj/kernel_symtab.d

clean:
	rm -rf *.o *.bin os.iso os_full.iso iso iso_full obj/*
	rm -rf kernel.sym src/kernel_symtab.c kernel_final.bin


run: os.iso
	@bash ./setup_tap.sh
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
	$(AUDIO_FLAGS) \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device rtl8139,netdev=net0,mac=BC:24:11:8F:DA:77 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-drive file=nvme_disk.img,format=raw,index=1,media=disk \
	-device $(VIRTIO_GPU_DISPLAY) \
	-serial file:serial.log

# WSL2-friendly: uses QEMU built-in user-mode NAT (no tap/iptables needed)
run-wsl: os.iso
	qemu-system-x86_64 -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
	$(AUDIO_FLAGS) \
	-netdev user,id=net0 \
	-device rtl8139,netdev=net0,mac=BC:24:11:8F:DA:77 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-drive file=nvme_disk.img,format=raw,index=1,media=disk \
	-device $(VIRTIO_GPU_DISPLAY) \
	-display gtk,zoom-to-fit=on \
	-serial file:serial.log


run-tap: os.iso
	sudo qemu-system-x86_64 \
		-machine q35 \
		-smp 2 \
		-cpu qemu64,+aes,+pni,+popcnt,+sse4.1,+sse4.2,+ssse3 \
		-m 1024 \
		-global ICH9-LPC.disable_s3=1 \
		-global ICH9-LPC.disable_s4=1 \
		-device qemu-xhci,id=xhci \
		-device usb-tablet,bus=xhci.0 \
		-device $(VIRTIO_VGA_DEV) \
		-device virtio-serial-pci \
		-device virtio-balloon-pci,free-page-reporting=on \
		-drive file=os.iso,media=cdrom,format=raw,readonly=on,index=2 \
		-serial file:serial.log \
		-drive file=sata_disk.img,format=raw,index=1 \
		-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
		-device rtl8139,netdev=net0,mac=BC:24:11:8F:DA:76

run-pcnet: os.iso
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 \
	$(AUDIO_FLAGS) \
	-netdev user,id=net0 \
	-device pcnet,netdev=net0,mac=52:54:00:12:34:57 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-drive file=nvme_disk.img,format=raw,index=1,media=disk \
	-serial file:serial.log \
	-vga vmware \
	-global vmware-svga.vgamem_mb=32

run-bridge: os.iso
	@echo "Bringing up tap0 bridged to uplink via setup_bridge.sh..."
	@echo "TIP: set UPLINK_IF=<your-nic> MOVE_UPLINK_IP=1 BR_NAME=br0 if needed."
	@echo "TIP: run 'sudo -v' first so sudo can prompt before Make/QEMU starts."
	@bash ./setup_bridge.sh
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 \
	$(AUDIO_FLAGS) \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-drive file=nvme_disk.img,format=raw,index=1,media=disk \
	-serial file:serial.log \
	-vga vmware \
	-global vmware-svga.vgamem_mb=32

debug: os.iso
	sudo qemu-system-x86_64 -s -S -boot d -cdrom os.iso -smp 1 -m 1G -M q35 \
	$(AUDIO_FLAGS) \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-drive file=nvme_disk.img,format=raw,index=1,media=disk \
	-serial file:serial.log \
	-vga vmware \
	-global vmware-svga.vgamem_mb=32 
debug-detailed: os.iso
	qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log -d int,mmu,guest_errors,cpu_reset -D qemu-debug.log

debug-gdb: os.iso
	qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log -s -S

# Debug build with function entry logging
debug-build: clean
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" os.iso

debug-run: debug-build
	qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 -serial file:serial.log \
	-vga vmware \
	-global vmware-svga.vgamem_mb=32

# USB-only test target (disables PS/2, uses USB for keyboard and mouse)
test-usb: CFLAGS += -DDISABLE_PS2_INPUT
test-usb: clean kernel_final.bin
	@echo "USB-only kernel built successfully!"
	@echo "This kernel will use USB for keyboard and mouse input instead of PS/2"

test-usb-iso: CFLAGS += -DDISABLE_PS2_INPUT  
test-usb-iso: clean os.iso
	@echo "USB-only ISO built successfully!"
	@echo "This ISO will use USB for keyboard and mouse input instead of PS/2"

test-usb-run: test-usb-iso
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 \
	$(AUDIO_FLAGS) \
	-netdev user,id=net0 \
	-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-drive file=nvme_disk.img,format=raw,index=1,media=disk \
	-serial file:serial.log \
	-device qemu-xhci -device usb-kbd -device usb-mouse \
	-vga vmware \
	-global vmware-svga.vgamem_mb=32

# SDL User Demo application
sdl_user_demo.exe: apps/sdl_user_demo.c include/SDL.h include/syscalls.h build_userapp.sh
	@echo "Building SDL User Demo application..."
	bash build_userapp.sh apps/sdl_user_demo.c sdl_user_demo.exe

# Telnet client ELF
telnet.exe: apps/telnet.c include/syscalls.h build_userapp.sh
	@echo "Building telnet client..."
	bash build_userapp.sh apps/telnet.c telnet.exe

# SDL test target
sdl-test: kernel_final.bin sdl_user_demo.exe os.iso
	@echo "SDL library integrated into kernel and user demo app built!"
	@echo "Use 'make run' to test the OS, then run './sdl_user_demo.exe' from the shell"

# Zeus Shell is now integrated into the kernel
# No separate executables needed

# Dedicated GPU test target with enhanced graphics settings
rungpu: os.iso
	@echo "Starting inDevOS with VMware SVGA-II GPU driver support"
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 1 -m 1G -M q35 \
	$(AUDIO_FLAGS) \
	-netdev user,id=net0 \
	-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-drive file=nvme_disk.img,format=raw,index=1,media=disk \
	-serial file:serial.log \
	-vga vmware \
	-global vmware-svga.vgamem_mb=64 \
	-global vmware-svga.vga-compatible=on \
	-monitor stdio

# Run with AC97 audio device for testing AC97 driver
run-audio: os.iso
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 2 -m 1G -M q35 \
	-device AC97 \
	-audiodev pa,id=speaker \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device rtl8139,netdev=net0,mac=BC:24:11:8F:DA:77 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-serial file:serial.log

# Run with xHCI USB 3.0 controller for testing xHCI driver
run-xhci: os.iso
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 2 -m 1G -M q35 \
	-device qemu-xhci,id=xhci \
	-device usb-kbd,bus=xhci.0 \
	-device usb-mouse,bus=xhci.0 \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device rtl8139,netdev=net0,mac=BC:24:11:8F:DA:77 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-serial file:serial.log

# Run with SMP (multiple CPUs) for testing SMP driver
run-smp: os.iso
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device rtl8139,netdev=net0,mac=BC:24:11:8F:DA:77 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-serial file:serial.log

# Run with all Phase 6 features enabled
run-phase6: os.iso
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
	-device AC97 \
	-device qemu-xhci,id=xhci \
	-device usb-kbd,bus=xhci.0 \
	-device usb-tablet,bus=xhci.0 \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device rtl8139,netdev=net0,mac=BC:24:11:8F:DA:77 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-serial file:serial.log

# Run with virtio-GPU for testing virtio-gpu driver
run-virtio-gpu: os.iso
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
	-device $(VIRTIO_GPU_PCI) \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device rtl8139,netdev=net0,mac=BC:24:11:8F:DA:77 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-serial file:serial.log

# Run with all Phase 4 features enabled (virtio-gpu, xHCI USB, SMP)
run-phase4: os.iso
	sudo qemu-system-x86_64 -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
	-device $(VIRTIO_GPU_DISPLAY) \
	-device qemu-xhci,id=xhci \
	-device usb-kbd,bus=xhci.0 \
	-device usb-mouse,bus=xhci.0 \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device rtl8139,netdev=net0,mac=BC:24:11:8F:DA:77 \
	-drive file=sata_disk.img,format=raw,index=0 \
	-serial file:serial.log

.PHONY: all clean debug-build debug-run rungpu test-usb test-usb-iso test-usb-run sdl-test run-audio run-xhci run-smp run-phase6 run-virtio-gpu run-phase4 install-iso

# Fetch herb-engine-C-3D as external third-party source files
fetch-herb-engine:
	bash scripts/fetch_herb_engine.sh

.PHONY: fetch-herb-engine

# User-space herb engine (requires host x86_64-linux-gnu-gcc; see docs/HERB_ENGINE_INDEVOS.md)
herb-engine.exe: apps/herb_engine_indevos.c build_userapp.sh include/herb_indevos_compat.h include/syscalls.h
	bash build_userapp.sh apps/herb_engine_indevos.c herb_engine.exe

.PHONY: herb-engine.exe
