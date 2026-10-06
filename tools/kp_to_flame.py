#!/usr/bin/env python3
"""Convert inDevOS profiler serial output to folded stacks / speedscope JSON.

Usage:
  # From a serial log that contains either:
  #   --- PROFILER FOLDED --- ... --- END FOLDED ---
  # or KP,v1,... sample lines from `prof dump`
  python3 tools/kp_to_flame.py serial.log -o out.folded
  python3 tools/kp_to_flame.py serial.log --speedscope -o out.speedscope.json

Then open out.speedscope.json at https://www.speedscope.app/
or:  flamegraph.pl out.folded > flame.svg
"""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path

KP_RE = re.compile(
    r"^KP,v1,(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)\s*$"
)
FOLDED_LINE_RE = re.compile(r"^(.+?)\s+(\d+)\s*$")
# serial.c prefixes lines like: [  69.388405] user[pid2];memcpy 1394
SERIAL_TS_RE = re.compile(r"^\[\s*\d+\.\d+\]\s*")


def parse_folded_section(text: str) -> Counter[str]:
    counts: Counter[str] = Counter()
    in_sec = False
    for line in text.splitlines():
        if "PROFILER FOLDED" in line:
            in_sec = True
            continue
        if "END FOLDED" in line:
            in_sec = False
            continue
        if not in_sec:
            continue
        line = SERIAL_TS_RE.sub("", line.strip())
        m = FOLDED_LINE_RE.match(line)
        if not m:
            continue
        counts[m.group(1)] += int(m.group(2))
    return counts


def parse_kp_samples(text: str) -> Counter[str]:
    """Rebuild shallow folded stacks from KP sample records.

    Layout after profiler update:
      kind=1 SAMPLE, unused=user_mode, arg0=rip, arg1=f0, arg2=f1, arg3=f2
    """
    counts: Counter[str] = Counter()
    for line in text.splitlines():
        m = KP_RE.match(line.strip())
        if not m:
            continue
        kind = int(m.group(1))
        if kind != 1:
            continue
        user_mode = int(m.group(2))
        pid = int(m.group(4))
        rip, f0, f1, f2 = (int(m.group(i)) for i in (7, 8, 9, 10))
        root = "user" if user_mode else "kernel"
        if pid:
            root = f"{root}[pid{pid}]"
        frames = [x for x in (f2, f1, f0, rip) if x]
        # addresses only — host has no kernel_symtab; prefer FOLDED section
        parts = [root] + [f"0x{a:x}" for a in frames]
        counts[";".join(parts)] += 1
    return counts


def to_speedscope(counts: Counter[str], name: str = "inDevOS") -> dict:
    """Minimal speedscope evented-profile compatible JSON (sampled)."""
    frames: list[dict] = []
    frame_index: dict[str, int] = {}

    def idx(sym: str) -> int:
        if sym not in frame_index:
            frame_index[sym] = len(frames)
            frames.append({"name": sym})
        return frame_index[sym]

    samples: list[list[int]] = []
    weights: list[int] = []
    for path, count in counts.items():
        stack = [idx(p) for p in path.split(";") if p]
        samples.append(stack)
        weights.append(count)

    return {
        "$schema": "https://www.speedscope.app/file-format-schema.json",
        "shared": {"frames": frames},
        "profiles": [
            {
                "type": "sampled",
                "name": name,
                "unit": "samples",
                "startValue": 0,
                "endValue": sum(weights),
                "samples": samples,
                "weights": weights,
            }
        ],
        "activeProfileIndex": 0,
        "exporter": "inDevOS kp_to_flame.py",
        "name": name,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("logfile", type=Path, help="Serial log / pasted dump")
    ap.add_argument("-o", "--output", type=Path, help="Output path")
    ap.add_argument(
        "--speedscope",
        action="store_true",
        help="Write speedscope JSON instead of folded stacks",
    )
    args = ap.parse_args()

    text = args.logfile.read_text(errors="replace")
    counts = parse_folded_section(text)
    if not counts:
        counts = parse_kp_samples(text)
    if not counts:
        raise SystemExit(
            "No FOLDED section or KP samples found. "
            "In the guest run: prof start 1 → doom → prof stop → prof flame"
        )

    out = args.output
    if args.speedscope:
        if out is None:
            out = args.logfile.with_suffix(".speedscope.json")
        out.write_text(json.dumps(to_speedscope(counts), indent=2))
        print(f"Wrote {out} ({len(counts)} stacks, {sum(counts.values())} samples)")
        print("Open at https://www.speedscope.app/")
    else:
        if out is None:
            out = args.logfile.with_suffix(".folded")
        lines = [f"{k} {v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1])]
        out.write_text("\n".join(lines) + "\n")
        print(f"Wrote {out} ({len(counts)} stacks, {sum(counts.values())} samples)")
        print("Top 15:")
        for k, v in list(sorted(counts.items(), key=lambda kv: -kv[1]))[:15]:
            pct = 100.0 * v / sum(counts.values())
            print(f"  {pct:5.1f}% {v:6d}  {k}")


if __name__ == "__main__":
    main()
