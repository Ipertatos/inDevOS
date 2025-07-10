#!/bin/bash
# build_userapp.sh - Build user applications for inDevOS
# Usage: ./build_userapp.sh myapp.c        (creates myapp.exe)
#        ./build_userapp.sh myapp.asm      (creates myapp.exe)

set -e

if [ $# -ne 1 ]; then
    echo "Usage: $0 <source_file>"
    echo "  Supports .c and .asm files"
    exit 1
fi

SRCFILE="$1"
BASENAME=$(basename "$SRCFILE" | sed 's/\.[^.]*$//')
OUTFILE="${BASENAME}.exe"

echo "Building $SRCFILE -> $OUTFILE"

case "$SRCFILE" in
    *.c)
        echo "Compiling C source with proper flags..."
        x86_64-linux-gnu-gcc \
            -nostdlib -static -fPIE -pie -ffreestanding \
            -Wl,--entry=_start \
            -o "$OUTFILE" "$SRCFILE"
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