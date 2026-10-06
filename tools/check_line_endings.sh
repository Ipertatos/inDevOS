#!/usr/bin/env bash
# Report line-ending conventions across src/ to keep edits consistent.
cd /mnt/c/osproj || exit 1
crlf=0
lf=0
for f in src/*.c src/*.h; do
    if grep -q $'\r' "$f"; then crlf=$((crlf + 1)); else lf=$((lf + 1)); fi
done
echo "src CRLF=$crlf LF=$lf"
for f in src/zeus.c src/cmd.c src/kernel.c src/term.c src/ac97.c src/smp.c src/stdio.h src/fat32.c; do
    printf '  %-18s CR lines: %s\n' "$f" "$(grep -c $'\r' "$f" || true)"
done
