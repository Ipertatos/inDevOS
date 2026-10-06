################################################################
#
# DOOM Makefile for inDevOS
# Adapted from Linux DOOM 1.10 Makefile
#
################################################################

CC = x86_64-linux-gnu-gcc
LD = x86_64-linux-gnu-ld

# Compiler flags for inDevOS userspace application
# Note: We include ../include for syscalls.h but NOT ../src to avoid conflicts
#
# Optimisation level.  The guest CPU is emulated (no KVM in WSL/QEMU-TCG), so
# unoptimised code costs several times more wall-clock than it would natively,
# but this is 1993 code with plenty of undefined behaviour and anything above
# -O0 miscompiles it (observed: user-mode #GP/#PF during W_Init in
# FindResponseFile / W_CheckNumForName / strupr once the binary is optimised).
# Keep -O0 and make the hot memory functions fast instead.
# -fno-strict-aliasing because DOOM does type punning.
#
# -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0: Ubuntu's gcc force-enables fortify at
# -O1+, which makes glibc's <string.h>/<stdio.h> emit inline definitions and
# __*_chk calls that clash with this freestanding app's own libc shim.
# __NO_INLINE__ likewise disables glibc's __USE_EXTERN_INLINES definitions
# (e.g. getchar in bits/stdio.h) that also only appear when optimising.
CFLAGS = -g -O0 -fno-strict-aliasing -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
         -D__NO_INLINE__ \
         -Wall -Wno-unused-parameter -Wno-unused-function \
         -Wno-unused-variable -fPIE -ffreestanding \
         -DNORMALUNIX -DINDVOS \
         -DINDEVOS_UNBUFFERED_STDOUT \
         -I. -I../include \
         -fno-stack-protector

# Linker flags for PIE executable (entry _start — kernel only sets argc/argv on stack)
LDFLAGS = -nostdlib -static -pie \
          -Wl,--entry=_start

# Libraries (none for freestanding)
LIBS = 

# Output directory
O = obj

# Create output directory
$(shell mkdir -p $(O))

# DOOM object files
OBJS = \
	$(O)/libc_shim.o	\
	$(O)/doomdef.o		\
	$(O)/doomstat.o		\
	$(O)/dstrings.o		\
	$(O)/i_system.o		\
	$(O)/i_sound.o		\
	$(O)/i_video.o		\
	$(O)/i_net.o		\
	$(O)/i_main.o		\
	$(O)/tables.o		\
	$(O)/f_finale.o		\
	$(O)/f_wipe.o		\
	$(O)/d_main.o		\
	$(O)/d_net.o		\
	$(O)/d_items.o		\
	$(O)/g_game.o		\
	$(O)/m_menu.o		\
	$(O)/m_misc.o		\
	$(O)/m_argv.o		\
	$(O)/m_bbox.o		\
	$(O)/m_fixed.o		\
	$(O)/m_swap.o		\
	$(O)/m_cheat.o		\
	$(O)/m_random.o		\
	$(O)/am_map.o		\
	$(O)/p_ceilng.o		\
	$(O)/p_doors.o		\
	$(O)/p_enemy.o		\
	$(O)/p_floor.o		\
	$(O)/p_inter.o		\
	$(O)/p_lights.o		\
	$(O)/p_map.o		\
	$(O)/p_maputl.o		\
	$(O)/p_plats.o		\
	$(O)/p_pspr.o		\
	$(O)/p_setup.o		\
	$(O)/p_sight.o		\
	$(O)/p_spec.o		\
	$(O)/p_switch.o		\
	$(O)/p_mobj.o		\
	$(O)/p_telept.o		\
	$(O)/p_tick.o		\
	$(O)/p_saveg.o		\
	$(O)/p_user.o		\
	$(O)/r_bsp.o		\
	$(O)/r_data.o		\
	$(O)/r_draw.o		\
	$(O)/r_main.o		\
	$(O)/r_plane.o		\
	$(O)/r_segs.o		\
	$(O)/r_sky.o		\
	$(O)/r_things.o		\
	$(O)/w_wad.o		\
	$(O)/wi_stuff.o		\
	$(O)/v_video.o		\
	$(O)/st_lib.o		\
	$(O)/st_stuff.o		\
	$(O)/hu_stuff.o		\
	$(O)/hu_lib.o		\
	$(O)/s_sound.o		\
	$(O)/z_zone.o		\
	$(O)/info.o		\
	$(O)/sounds.o

all: doom

doom: $(OBJS)
	@echo "Linking DOOM..."
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o doom.exe $(LIBS)
	@echo "DOOM built successfully!"
	@echo ""
	@echo "To run DOOM, you need:"
	@echo "1. Copy doom.exe to your disk image using putfilesinsata.sh"
	@echo "2. Copy DOOM1.WAD (shareware) or DOOM.WAD to the disk"
	@echo "3. Boot inDevOS and run: exec doom.exe"
	@echo ""
	@echo "Note: You'll need to obtain DOOM WAD files separately."
	@echo "Shareware DOOM1.WAD can be downloaded from:"
	@echo "https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad"

$(O)/%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(O)
	rm -f doom.exe doom

install: doom
	@echo "Copying doom.exe to disk image..."
	@if [ -f ../putfilesinsata.sh ]; then \
		../putfilesinsata.sh doom.exe; \
		echo "doom.exe copied to disk image!"; \
	else \
		echo "Warning: putfilesinsata.sh not found"; \
		echo "Copy doom.exe manually to your disk image"; \
	fi

.PHONY: all clean install

# Note: This builds DOOM as a freestanding binary that can run in inDevOS
# You'll need to ensure the linker script and startup code are compatible
