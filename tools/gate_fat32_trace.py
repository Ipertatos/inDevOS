#!/usr/bin/env python3
"""Gate per-operation FAT32 chatter behind ktrace().

`printf("fat32: Opened file ...")` runs on every open and costs a console write
(15-40 ms emulated); with a dozen operations per program launch it dominated
start-up time.  Errors and mount-time messages keep printf().
"""
path = "src/fat32.c"
src = open(path, encoding="utf-8").read()

subs = [
    'printf("fat32: Opened file ',
    'printf("fat32: Created empty file ',
    'printf("fat32: Deleted file ',
    'printf("fat32: Created directory ',
    'printf("fat32: Directory grew',
]

changed = 0
for needle in subs:
    if needle in src:
        src = src.replace(needle, "ktrace(" + needle[len("printf("):])
        changed += 1
    else:
        print("not found:", needle)

open(path, "w", encoding="utf-8").write(src)
print("rewrote", changed, "pattern(s) in", path)
