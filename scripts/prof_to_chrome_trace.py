#!/usr/bin/env python3
"""
Convert inDevOS profiler serial dump lines (prefix KP,v1,) to Chrome Trace Event JSON.

Usage:
  grep '^KP,v1,' serial.log | python3 scripts/prof_to_chrome_trace.py > trace.json

Then open chrome://tracing or https://ui.perfetto.dev/ and load trace.json
"""

import json
import re
import sys

SERIAL_TS_PREFIX = re.compile(r"^\[\s*\d+\.\d{6}\]\s*")

KIND_SAMPLE = 1
KIND_SC_ENTER = 2
KIND_SC_EXIT = 3
KIND_SCHED = 4
KIND_FRAME = 5

KIND_NAMES = {
    KIND_SAMPLE: "sample",
    KIND_SC_ENTER: "sc_enter",
    KIND_SC_EXIT: "sc_exit",
    KIND_SCHED: "sched",
    KIND_FRAME: "frame",
}


def parse_lines(lines):
    rows = []
    for line in lines:
        line = line.strip()
        line = SERIAL_TS_PREFIX.sub("", line, count=1)
        if not line.startswith("KP,v1,"):
            continue
        parts = line.split(",")
        # Format: KP,v1, + 10 numeric fields = 12 CSV tokens.
        # Legacy bug: one extra ",0" (13 tokens) — trim trailing field.
        if len(parts) > 12 and parts[0] == "KP" and parts[1] == "v1":
            parts = parts[:12]
        if len(parts) != 12:
            continue
        try:
            _, _, kind, unused, cpu, pid, tid, ts_us, a0, a1, a2, a3 = parts
            rows.append(
                {
                    "kind": int(kind),
                    "unused": int(unused),
                    "cpu": int(cpu),
                    "pid": int(pid),
                    "tid": int(tid),
                    "ts_us": int(ts_us),
                    "arg0": int(a0),
                    "arg1": int(a1),
                    "arg2": int(a2),
                    "arg3": int(a3),
                }
            )
        except ValueError:
            continue
    rows.sort(key=lambda r: r["ts_us"])
    return rows


def to_chrome(rows):
    if not rows:
        return {"traceEvents": []}
    t0 = rows[0]["ts_us"]
    events = []

    for r in rows:
        ts = r["ts_us"] - t0
        k = r["kind"]
        name = KIND_NAMES.get(k, f"kind{k}")
        base = {
            "cat": "profiler",
            "name": name,
            "ph": "i",
            "ts": ts,
            "pid": r["cpu"],
            "tid": r["tid"],
            "args": {
                "kern_pid": r["pid"],
                "unused": r["unused"],
                "a0": r["arg0"],
                "a1": r["arg1"],
                "a2": r["arg2"],
                "a3": r["arg3"],
            },
        }

        if k == KIND_SAMPLE:
            base["name"] = f"rip 0x{r['arg0']:x}"

        elif k == KIND_SC_EXIT:
            dur = max(1.0, float(r["arg1"]))
            base["ph"] = "X"
            base["dur"] = dur
            base["name"] = f"syscall {r['arg0']} ({dur:.1f}us)"

        elif k == KIND_SCHED:
            base["name"] = (
                f"sched pid{r['pid']}-> pid{r['arg1']} "
                f"th{r['tid']}->th{r['arg0']}"
            )

        elif k == KIND_FRAME:
            base["name"] = "SDL_RenderPresent"

        events.append(base)

    return {"traceEvents": events}


def main():
    data = sys.stdin.read().splitlines()
    rows = parse_lines(data)
    if not rows and data:
        has_kp = any(s.strip().startswith("KP,v1,") for s in data)
        if has_kp:
            print(
                "prof_to_chrome_trace: KP,v1 lines found but none parsed "
                "(update script if kernel dump format changed).",
                file=sys.stderr,
            )
        else:
            print(
                "prof_to_chrome_trace: no KP,v1 input (pipe: "
                "grep '^KP,v1,' serial.log | …).",
                file=sys.stderr,
            )
    out = to_chrome(rows)
    json.dump(out, sys.stdout, indent=None)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
