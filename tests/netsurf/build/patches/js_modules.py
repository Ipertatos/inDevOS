#!/usr/bin/env python3
"""ES modules for the QuickJS backend: type=module (case-insensitive) runs via
js_exec_module, external modules are deferred, <script type=importmap>
goes to js_importmap, nomodule scripts are skipped.

Usage: js_modules.py <path/to/netsurf-all-3.11>   (idempotent; applies js_modules.diff)"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nsdiff

if len(sys.argv) != 2:
    sys.exit(__doc__)
nsdiff.apply('js_modules', sys.argv[1], [])
