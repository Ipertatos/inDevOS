#!/usr/bin/env bash
# Fail if zeus.c's command registry (zeus_command_names) and the names actually
# handled by zeus_execute_command() ever drift apart.
#
#   tools/check_zeus_commands.sh          # report + exit 1 on mismatch
#
# Both lists live in src/zeus.c - the dispatcher's `kstrcmp(command, "x")`
# branches and the registry used for tab completion / multi-match listing.
set -u
cd "$(dirname "$0")/.." || exit 1

ZEUS=src/zeus.c
[ -f "$ZEUS" ] || { echo "missing $ZEUS"; exit 1; }

python3 - "$ZEUS" <<'PY'
import re
import sys

src = open(sys.argv[1], encoding="utf-8", errors="replace").read()

# 1. Names the dispatcher compares against.
dispatch = set(re.findall(r'kstrcmp\(command,\s*"([A-Za-z0-9_]+)"\)', src))

# 2. Names in the zeus_command_names[] registry.
m = re.search(r'zeus_command_names\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
if not m:
    print("could not find zeus_command_names[] in", sys.argv[1])
    sys.exit(1)
registry = set(re.findall(r'"([A-Za-z0-9_]+)"', m.group(1)))

# 3. Programs on disk that the shell launches (zeus_fs_programs) are command
#    names too, even though the dispatcher never sees them.
m = re.search(r'zeus_fs_programs\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
fs_progs = set(re.findall(r'"([A-Za-z0-9_]+)"', m.group(1))) if m else set()

expected = dispatch | fs_progs

missing = sorted(expected - registry)   # dispatchable but not completable
extra = sorted(registry - expected)     # offered but not dispatchable

print(f"dispatch={len(dispatch)} fs_programs={len(fs_progs)} registry={len(registry)}")
if missing:
    print("MISSING from zeus_command_names[] (would not tab-complete):")
    for n in missing:
        print("   ", n)
if extra:
    print("EXTRA in zeus_command_names[] (completes to a non-command):")
    for n in extra:
        print("   ", n)

if missing or extra:
    sys.exit(1)
print("zeus command registry matches the dispatcher")
PY
