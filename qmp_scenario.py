#!/usr/bin/env python3
"""Drive a running QEMU over QMP through a scripted sequence of steps.

Usage: qmp_scenario.py <port> <out-prefix> <step> [<step> ...]
Each <step> is  wait:SECONDS  |  type:TEXT  |  shot:NAME  |  key:QCODE
"""
import json
import socket
import sys
import time

port = int(sys.argv[1])
prefix = sys.argv[2]
steps = sys.argv[3:]

QCODE = {" ": "spc", "\n": "ret", "-": "minus", ".": "dot",
         "/": "slash", ",": "comma", "=": "equal",
         "'": "apostrophe",
         ":": ["shift", "semicolon"]}
# Characters that need Shift held down (US layout).
SHIFTED = {"_": "minus", "+": "equal", "*": "8", "(": "9", ")": "0",
           "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
           "&": "7", "?": "slash", "<": "comma", ">": "dot", '"': "apostrophe",
           "{": "bracket_left", "}": "bracket_right", "|": "backslash"}


class Qmp:
    def __init__(self):
        for _ in range(60):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                time.sleep(0.5)
        else:
            raise SystemExit("could not connect to QMP")
        self.f = self.s.makefile("rw")
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("QMP closed")
            line = line.strip()
            if line:
                return json.loads(line)

    def cmd(self, name, args=None):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        return self._read()

    def type_text(self, text):
        for ch in text:
            if ch.isalpha() and ch.isupper():
                # QEMU qcodes are lowercase key names; uppercase needs Shift.
                keys = ["shift", ch.lower()]
            elif ch in SHIFTED:
                keys = ["shift", SHIFTED[ch]]
            else:
                k = QCODE.get(ch, ch)
                keys = k if isinstance(k, list) else [k]
            self.cmd("send-key", {"keys": [{"type": "qcode", "data": x} for x in keys],
                                  "hold-time": 40})
            time.sleep(0.08)

    def shot(self, name):
        path = f"{prefix}-{name}.png"
        r = self.cmd("screendump", {"filename": path, "format": "png"})
        print(("ok   " if "error" not in r else "FAIL ") + path)


def main():
    q = Qmp()
    t0 = time.monotonic()
    for step in steps:
        kind, _, arg = step.partition(":")
        if kind == "wait":
            time.sleep(float(arg))
        elif kind == "type":
            # Send a newline first: the shell sometimes swallows the first
            # keystroke that arrives while it is printing the prompt.
            q.cmd("send-key", {"keys": [{"type": "qcode", "data": "ret"}], "hold-time": 40})
            time.sleep(0.4)
            print(f"[{time.monotonic() - t0:7.1f}s] type: {arg}")
            q.type_text(arg + "\n")
            time.sleep(0.6)
        elif kind == "raw":
            # Type without pressing Enter (for Tab-completion tests).
            print(f"[{time.monotonic() - t0:7.1f}s] raw: {arg}")
            q.type_text(arg)
            time.sleep(0.4)
        elif kind == "shot":
            q.shot(arg)
        elif kind == "click":
            xs, _, ys = arg.partition(",")
            x, y = int(xs), int(ys)
            print(f"[{time.monotonic() - t0:7.1f}s] click: {x},{y}")
            ax = max(0, min(32767, x * 32767 // 1920))
            ay = max(0, min(32767, y * 32767 // 1080))
            q.cmd("input-send-event", {"events": [
                {"type": "abs", "data": {"axis": "x", "value": ax}},
                {"type": "abs", "data": {"axis": "y", "value": ay}},
            ]})
            time.sleep(0.08)
            q.cmd("input-send-event", {"events": [
                {"type": "btn", "data": {"down": True, "button": "left"}},
            ]})
            time.sleep(0.08)
            q.cmd("input-send-event", {"events": [
                {"type": "btn", "data": {"down": False, "button": "left"}},
            ]})
            time.sleep(0.4)
        elif kind == "key":
            # comma-separated keys are pressed together, e.g. key:ctrl,c
            keys = [k.strip() for k in arg.split(",") if k.strip()]
            q.cmd("send-key", {"keys": [{"type": "qcode", "data": k} for k in keys],
                               "hold-time": 60})
        else:
            raise SystemExit("unknown step " + step)
    q.cmd("quit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
