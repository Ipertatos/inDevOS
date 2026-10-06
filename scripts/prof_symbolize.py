#!/usr/bin/env python3
"""Flat profile from `prof dump` samples.

Usage: prof_symbolize.py <serial.log> [kernel_final.bin] [--callers]

Resolves the leaf RIP of every SAMPLE record (KP,v1,1,...) against the kernel
symbol table (nm -n) and prints the hottest functions.  --callers also counts
the first caller frame, which helps when the leaf is memcpy/memset.
"""
import bisect
import subprocess
import sys
from collections import Counter


def load_symbols(binary):
    out = subprocess.run(["nm", "-n", binary], capture_output=True, text=True,
                         check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def resolve(addrs, names, a):
    i = bisect.bisect_right(addrs, a) - 1
    return names[i] if i >= 0 else hex(a)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    callers = "--callers" in sys.argv
    log = args[0]
    binary = args[1] if len(args) > 1 else "kernel_final.bin"
    addrs, names = load_symbols(binary)

    leaf, caller = Counter(), Counter()
    total = 0
    with open(log, errors="replace") as f:
        for line in f:
            if not line.startswith("KP,v1,1,"):
                continue
            tok = line.strip().split(",")
            if len(tok) < 12:
                continue
            rip, c1 = int(tok[8]), int(tok[9])
            total += 1
            leaf[resolve(addrs, names, rip)] += 1
            if c1:
                caller[resolve(addrs, names, c1)] += 1

    print(f"{total} samples")
    for name, n in leaf.most_common(25):
        print(f"{n:6d} {100.0 * n / max(total, 1):5.1f}%  {name}")
    if callers:
        print("\n-- first caller --")
        for name, n in caller.most_common(15):
            print(f"{n:6d} {100.0 * n / max(total, 1):5.1f}%  {name}")


if __name__ == "__main__":
    main()
