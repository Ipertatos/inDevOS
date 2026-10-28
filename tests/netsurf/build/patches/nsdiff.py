#!/usr/bin/env python3
"""Shared helper for the QuickJS / CSS Grid patch scripts.

apply(name, root, copies): copy the listed new files into the tree, then
apply patches/<name>.diff (unified, -p1 relative to netsurf-all-3.11).
Idempotent: an already-applied diff is detected with a reverse dry run.
Needs GNU patch on the host."""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _patch(root, diff, *args):
    return subprocess.run(['patch', '-p1', '-d', root, '-i', diff, '-s', '-f',
                           '--no-backup-if-mismatch'] + list(args),
                          capture_output=True, text=True)


def apply(name, root, copies=()):
    root = os.path.abspath(root)
    if not os.path.isdir(os.path.join(root, 'netsurf')):
        sys.exit(f'{name}: {root} is not a netsurf-all-3.11 tree')
    for src, dst in copies:
        src = os.path.join(HERE, src)
        dst = os.path.join(root, dst)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.exists(dst) or open(src, 'rb').read() != open(dst, 'rb').read():
            shutil.copyfile(src, dst)
            print(f'{name}: copied {os.path.relpath(dst, root)}')
    diff = os.path.join(HERE, name + '.diff')
    if _patch(root, diff, '-R', '--dry-run').returncode == 0:
        print(f'{name}: already applied')
        return
    r = _patch(root, diff, '--dry-run')
    if r.returncode != 0:
        sys.exit(f'{name}: {name}.diff does not apply:\n{r.stdout}{r.stderr}')
    r = _patch(root, diff)
    if r.returncode != 0:
        sys.exit(f'{name}: patch failed:\n{r.stdout}{r.stderr}')
    print(f'{name}: patched')
