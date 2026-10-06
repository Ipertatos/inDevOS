#!/usr/bin/env python3
"""Gate hot-path loader chatter behind ktrace().

The per-launch printf()s (ABI stack, launch, Ring-3 entry, exit) cost a console
write each; with tracing off they must be free.  Error paths keep printf().
"""
import sys

path = "src/exec.c"
src = open(path, encoding="utf-8").read()

# Each entry: printf(  ->  ktrace(   for the same format string.
subs = [
    'printf("exec: ABI stack @',
    'printf("exec: loaded %d ELF symbol(s)',
    'printf("exec: launching ',
    'printf("exec: entering Ring 3 rip=',
    'printf("exec: process ',
    'printf("exec_spawn: ',
    'printf("Spawned ',
]

changed = 0
for needle in subs:
    if needle in src:
        src = src.replace(needle, "ktrace(" + needle[len("printf("):], 1)
        changed += 1
    else:
        print("not found:", needle)

open(path, "w", encoding="utf-8").write(src)
print("rewrote", changed, "call(s) in", path)
