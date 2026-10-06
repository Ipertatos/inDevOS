#!/bin/bash
# build_userapp.sh - Build user applications for inDevOS
# Usage: ./build_userapp.sh myapp.c [output.exe]        (creates myapp.exe or custom name)
#        ./build_userapp.sh myapp.asm [output.exe]      (creates myapp.exe or custom name)

set -e

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "Usage: $0 <source_file> [output_file]"
    echo "  Supports .c and .asm files"
    echo "  If output_file not specified, uses <source_basename>.exe"
    exit 1
fi

SRCFILE="$1"
if [ $# -eq 2 ]; then
    OUTFILE="$2"
else
    BASENAME=$(basename "$SRCFILE" | sed 's/\.[^.]*$//')
    OUTFILE="${BASENAME}.exe"
fi

echo "Building $SRCFILE -> $OUTFILE"

case "$SRCFILE" in
    *.c)
        echo "Compiling C source with proper flags..."
        EXTRA_CFLAGS="-I."
        EXTRA_LDFLAGS=""
        # herb-engine port: libm (sin/cos/sqrt), project includes
        case "$SRCFILE" in
            *herb_engine_indevos.c)
                # SSE (movaps) on stack locals needs 16-byte RSP; iretq/_start entry can leave
                # the stack misaligned vs what GCC assumes — realign at every function entry.
                # -O3: software rasterization + qsort on indices; -O0 was ~1–3 fps on typical setups.
                EXTRA_CFLAGS="-I. -Iinclude -O3 -mstackrealign -fno-builtin-qsort -fno-builtin-sin -fno-builtin-cos -fno-builtin-sqrt -fno-builtin-floor"
                EXTRA_LDFLAGS="-lgcc"
                ;;
        esac
        x86_64-linux-gnu-gcc \
            -nostdlib -static -fPIE -pie -ffreestanding \
            $EXTRA_CFLAGS \
            -Wl,--entry=_start \
            -o "$OUTFILE" "$SRCFILE" $EXTRA_LDFLAGS
        strip "$OUTFILE"
        echo "Built ELF executable: $OUTFILE"
        ;;
    *.asm)
        echo "Assembling ASM source..."
        if grep -q "db 'EXE'" "$SRCFILE"; then
            # Has custom header embedded
            nasm -f bin -o "$OUTFILE" "$SRCFILE"
        else
            # Need to add custom header
            nasm -f bin -o "${BASENAME}.raw" "$SRCFILE"
            printf 'EXE!\x08\x00\x00\x00' > header.tmp
            cat header.tmp "${BASENAME}.raw" > "$OUTFILE"
            rm header.tmp "${BASENAME}.raw"
        fi
        echo "Built flat binary executable: $OUTFILE"
        ;;
    *)
        echo "Unsupported file type: $SRCFILE"
        echo "Supported: .c .asm"
        exit 1
        ;;
esac

echo "Copying to disk image..."
if [ -f putfilesinsata.sh ]; then
    ./putfilesinsata.sh "$OUTFILE"
else
    echo "Warning: putfilesinsata.sh not found, not copying to disk"
fi

echo "Done! Use 'exec $OUTFILE' in inDevOS" 