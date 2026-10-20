#!/usr/bin/env bash
# Build os_install.iso: Limine-bootable hybrid ISO with a FAT32 root partition.
#
# The kernel mounts device 0 as FAT32 (see fat32_mount / find_first_fat32_partition_lba).
# This image reuses the os.iso Limine layout and appends a type-0x0C FAT32 partition
# the kernel will mount as /, packed the same way apps/build_apps.sh fills sata_disk.img
# (mtools, no sudo). Debug/test apps are not copied.
#
# One command from Windows (the machine that uses build_windows.ps1):
#     .\make_install_iso.ps1
# From WSL or Linux, in the repo root:
#     bash ./make_install_iso.sh
# Makefile:  make install-iso
set -euo pipefail

cd "$(dirname "$0")"

OUT="os_install.iso"
ROOTFS="iso_rootfs.img"
STAGING="iso_install"
ROOTFS_MB=80

# Real userspace tools (apps/*.c). Not demos or tests
# (udp_test, tcp_test, sdl_user_demo, example_widgets_demo, herb_engine, ...).
UTILS=(
    ls sh cat clear cp date echo grep head help
    mkdir mv neofetch pwd rm stopwatch touch wc
)

die() { echo "make_install_iso: $*" >&2; exit 1; }

command -v mformat >/dev/null || die "mformat (mtools) is required to pack the FAT32 rootfs"
command -v mcopy >/dev/null || die "mcopy (mtools) is required"
command -v mmd >/dev/null || die "mmd (mtools) is required"
command -v xorriso >/dev/null || die "xorriso is required (same tool that builds os.iso)"

[[ -f kernel_final.bin ]] || die "kernel_final.bin not found. Build the kernel first (build_windows.ps1 or make kernel_final.bin)."
[[ -f limine.cfg ]] || die "limine.cfg not found"
for f in limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI; do
    [[ -f "limine/$f" ]] || die "missing limine/$f"
done
[[ -f doom/doom.exe ]] || die "doom/doom.exe not found"
[[ -f doom1.wad ]] || die "doom1.wad not found"

for u in "${UTILS[@]}"; do
    [[ -f "apps/${u}.exe" ]] || die "missing apps/${u}.exe (build userspace with apps/build_apps.sh)"
done

LIMINE_BIN=""
if [[ -x limine/limine ]]; then
    LIMINE_BIN="limine/limine"
elif [[ -f limine/limine.exe ]]; then
    LIMINE_BIN="limine/limine.exe"
else
    die "limine host tool not found (limine/limine or limine/limine.exe)"
fi

echo ">>> FAT32 rootfs (${ROOTFS_MB} MiB, label INDEVOS)"
rm -f "$ROOTFS"
dd if=/dev/zero of="$ROOTFS" bs=1M count="$ROOTFS_MB" status=none
# Superfloppy FAT32. The kernel reads the boot sector at the partition LBA,
# so this image is valid both raw and as an appended 0x0C partition.
mformat -i "$ROOTFS" -F -v INDEVOS ::

mmd -i "$ROOTFS" ::/bin
mmd -i "$ROOTFS" ::/install

echo ">>> installing userspace utils into /bin"
for u in "${UTILS[@]}"; do
    mcopy -o -i "$ROOTFS" "apps/${u}.exe" "::/bin/${u}.exe"
    echo "    /bin/${u}.exe"
done

# Linux-ABI test programs (static musl), built by tests/musl/build_musl_tests.sh.
# Optional: skipped when they have not been built.
MUSL_TESTS=(hello_musl musltest)
# fbdev smoke test (tests/fbdev/fbgrad.c), same install rule.
[[ -f tests/fbdev/build/fbgrad.exe ]] && mcopy -o -i "$ROOTFS" tests/fbdev/build/fbgrad.exe ::/bin/fbgrad.exe && echo "    /bin/fbgrad.exe (static musl, fbdev test)"
# evdev checker (tests/evdev/evdump.c), same install rule.
[[ -f tests/evdev/build/evdump.exe ]] && mcopy -o -i "$ROOTFS" tests/evdev/build/evdump.exe ::/bin/evdump.exe && echo "    /bin/evdump.exe (static musl, evdev test)"
# uinput fake drivers (tests/uinput/fakemouse.c, fakekbd.c), same install rule.
for t in fakemouse fakekbd faketablet; do
    [[ -f tests/uinput/build/${t}.exe ]] && mcopy -o -i "$ROOTFS" tests/uinput/build/${t}.exe ::/bin/${t}.exe && echo "    /bin/${t}.exe (static musl, uinput test)"
done
for t in "${MUSL_TESTS[@]}"; do
    if [[ -f "tests/musl/build/${t}.exe" ]]; then
        mcopy -o -i "$ROOTFS" "tests/musl/build/${t}.exe" "::/bin/${t}.exe"
        echo "    /bin/${t}.exe (static musl, Linux ABI)"
    fi
done

# Static BusyBox (musl). Prefer tests/busybox/build/busybox.exe.
if [[ -f tests/busybox/build/busybox.exe ]]; then
    mcopy -o -i "$ROOTFS" tests/busybox/build/busybox.exe ::/bin/busybox.exe
    echo "    /bin/busybox.exe (static musl BusyBox)"
fi

# Networking config + CA bundle (HTTPS cert validation) + optional static curl.
NETTMP=$(mktemp -d)
printf "nameserver 10.0.2.3\n" > "$NETTMP/resolv.conf"
printf "127.0.0.1 localhost\n" > "$NETTMP/hosts"
mmd -D s -i "$ROOTFS" ::/etc ::/etc/ssl ::/etc/ssl/certs 2>/dev/null || true
mcopy -o -i "$ROOTFS" "$NETTMP/resolv.conf" ::/etc/resolv.conf
mcopy -o -i "$ROOTFS" "$NETTMP/hosts" ::/etc/hosts
# Desktop file associations (src/launch.c has the same built-in default).
mmd -D s -i "$ROOTFS" ::/etc/indevos 2>/dev/null || true
mcopy -o -i "$ROOTFS" config/open-with ::/etc/indevos/open-with
if [[ -f /etc/ssl/certs/ca-certificates.crt ]]; then
    mcopy -o -i "$ROOTFS" /etc/ssl/certs/ca-certificates.crt ::/etc/ssl/certs/ca-certificates.crt
    echo "    /etc/ssl/certs/ca-certificates.crt (CA bundle)"
fi
# Static musl curl (e.g. stunnel/static-curl curl-linux-x86_64-musl) at tests/curl/curl
if [[ -f tests/curl/curl ]]; then
    mcopy -o -i "$ROOTFS" tests/curl/curl ::/bin/curl.exe
    echo "    /bin/curl.exe (static musl curl, HTTPS)"
fi
rm -rf "$NETTMP"

# Static TinyCC + musl sysroot (headers, crt, libc.a) for on-OS C compile.
TCC_STAGE=""
if [[ -f tests/tcc/stage.path ]]; then
    TCC_STAGE=$(cat tests/tcc/stage.path)
elif [[ -L tests/tcc/stage ]]; then
    TCC_STAGE=$(readlink -f tests/tcc/stage)
elif [[ -x tests/tcc/stage/bin/tcc.exe ]]; then
    TCC_STAGE=tests/tcc/stage
fi
# TinyCC is configured with sysincludepaths /usr/include:/usr/lib/tcc/include,
# libpaths /usr/lib:/usr/lib/tcc and crtprefix /usr/lib (tests/tcc/build_tcc.sh),
# so the installed tree needs exactly these. Refuse a half-populated stage.
TCC_RUNTIME=(
    usr/include/stdio.h usr/include/stdlib.h usr/include/string.h
    usr/include/inttypes.h usr/include/stdio_ext.h usr/include/sys/types.h
    usr/include/bits/alltypes.h usr/lib/tcc/include/stddef.h
    usr/lib/tcc/include/stdarg.h usr/lib/tcc/libtcc1.a
    usr/lib/crt1.o usr/lib/crti.o usr/lib/crtn.o usr/lib/libc.a
)
if [[ -n "$TCC_STAGE" && -x "$TCC_STAGE/bin/tcc.exe" ]]; then
    for f in "${TCC_RUNTIME[@]}"; do
        [[ -f "$TCC_STAGE/$f" ]] || die "TCC stage $TCC_STAGE lacks $f (rerun tests/tcc/build_tcc.sh)"
    done
    mmd -i "$ROOTFS" ::/usr 2>/dev/null || true
    mmd -i "$ROOTFS" ::/usr/lib 2>/dev/null || true
    mmd -i "$ROOTFS" ::/usr/lib/tcc 2>/dev/null || true
    mmd -i "$ROOTFS" ::/usr/include 2>/dev/null || true
    mmd -i "$ROOTFS" ::/tmp 2>/dev/null || true
    mmd -i "$ROOTFS" ::/home 2>/dev/null || true
    mmd -i "$ROOTFS" ::/home/user 2>/dev/null || true
    mcopy -o -i "$ROOTFS" "$TCC_STAGE/bin/tcc.exe" ::/bin/tcc.exe
    mcopy -o -s -i "$ROOTFS" "$TCC_STAGE/usr" ::/
    if ls "$TCC_STAGE"/home/user/*.c >/dev/null 2>&1; then
        mcopy -o -i "$ROOTFS" "$TCC_STAGE"/home/user/*.c ::/home/user/
    fi
    # Self-hosting extras: static musl GNU make and a tiny make demo.
    if [[ -f "$TCC_STAGE/bin/make.exe" ]]; then
        mcopy -o -i "$ROOTFS" "$TCC_STAGE/bin/make.exe" ::/bin/make.exe
        echo "    /bin/make.exe (static musl GNU make)"
    fi
    if [[ -d "$TCC_STAGE/home/user/mk" ]]; then
        mcopy -o -s -i "$ROOTFS" "$TCC_STAGE/home/user/mk" ::/home/user/
    fi
    echo "    /bin/tcc.exe + /usr/{include,lib} from $TCC_STAGE ($(find "$TCC_STAGE/usr" -type f | wc -l) files)"
    # mtools writes long names (stdio_ext.h, bits/alltypes.h, ...) as VFAT LFN
    # entries; check a few made it with their real names.
    for f in usr/include/stdio_ext.h usr/include/bits/alltypes.h usr/lib/tcc/libtcc1.a usr/lib/crt1.o; do
        mdir -i "$ROOTFS" "::/$f" >/dev/null 2>&1 || die "rootfs is missing /$f after copying the TCC stage"
    done
else
    echo "    WARNING: no TinyCC stage (tests/tcc/stage.path or tests/tcc/stage); /bin/tcc.exe and its headers are NOT in the image"
fi


echo ">>> installing Doom (one binary, one WAD)"
mcopy -o -i "$ROOTFS" doom/doom.exe ::/bin/doom.exe
mcopy -o -i "$ROOTFS" doom1.wad ::/doom1.wad

echo ">>> installing Limine payload for the in-OS installer (/install)"
mcopy -o -i "$ROOTFS" kernel_final.bin ::/install/boot.bin
mcopy -o -i "$ROOTFS" limine.cfg ::/install/limine.cfg
mcopy -o -i "$ROOTFS" limine/limine-bios.sys ::/install/limine-bios.sys
# UEFI loader: the installer copies it to /EFI/BOOT/BOOTX64.EFI on the target.
mcopy -o -i "$ROOTFS" limine/BOOTX64.EFI ::/install/BOOTX64.EFI

readme="$(mktemp)"
cat > "$readme" << 'ENDREADME'
inDevOS install root
====================
FAT32 volume the kernel mounts as /.
Programs: /bin/<name>.exe   (ls, sh, cat, and the other shell tools)
Linux:    /bin/hello_musl.exe, /bin/musltest.exe, /bin/busybox.exe, /bin/tcc.exe
Doom:     /bin/doom.exe
WAD:      /doom1.wad
Installer payload: /install/ (kernel, limine.cfg, limine-bios.sys, BOOTX64.EFI).
The in-OS install command copies this whole root, then places the
kernel and Limine under /boot so the target disk boots on its own.
ENDREADME
mcopy -o -i "$ROOTFS" "$readme" ::/README.txt
rm -f "$readme"

echo ">>> staging Limine ISO tree (same layout as os.iso)"
rm -rf "$STAGING"
mkdir -p "$STAGING/boot/limine" "$STAGING/EFI/BOOT"
cp kernel_final.bin "$STAGING/boot/boot.bin"
cp limine.cfg \
   limine/limine-bios.sys \
   limine/limine-bios-cd.bin \
   limine/limine-uefi-cd.bin \
   "$STAGING/boot/limine/"
cp limine/BOOTX64.EFI "$STAGING/EFI/BOOT/"
if [[ -f limine/BOOTIA32.EFI ]]; then
    cp limine/BOOTIA32.EFI "$STAGING/EFI/BOOT/"
fi

echo ">>> xorriso (Limine El Torito + appended FAT32 partition)"
rm -f "$OUT"
xorriso -as mkisofs \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    -append_partition 2 0x0c "$ROOTFS" \
    "$STAGING" -o "$OUT"

echo ">>> limine bios-install"
"$LIMINE_BIN" bios-install "$OUT"

rm -rf "$STAGING"

python3 - << 'ENDCHECK'
import struct, sys
path = "os_install.iso"
data = open(path, "rb").read(512)
if data[510:512] != b"\x55\xaa":
    sys.exit("MBR signature missing on os_install.iso")
found = False
for i in range(4):
    e = data[446 + i * 16:446 + (i + 1) * 16]
    typ = e[4]
    lba, nsec = struct.unpack_from("<II", e, 8)
    print(f"    MBR part {i}: type=0x{typ:02x} lba={lba} sectors={nsec}")
    if typ in (0x0B, 0x0C) and lba and nsec:
        found = True
if not found:
    sys.exit("no FAT32 (0x0B/0x0C) partition in the ISO MBR; the kernel would not mount this rootfs")
ENDCHECK

bytes=$(stat -c %s "$OUT" 2>/dev/null || wc -c < "$OUT")
echo ">>> $OUT ($bytes bytes)"
echo ">>> rootfs listing:"
mdir -i "$ROOTFS" -/ :: | sed 's/^/    /'
