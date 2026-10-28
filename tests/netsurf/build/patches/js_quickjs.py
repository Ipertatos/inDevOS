#!/usr/bin/env python3
"""QuickJS JavaScript backend for NetSurf 3.11: NETSURF_USE_QUICKJS feature switch
(-lquickjs), javascript/Makefile picks quickjs/ over duktape/, copies the
bindings (../quickjs/qjs.c, dom.js, Makefile) to
content/handlers/javascript/quickjs/, KeyboardEvent.key names
(Enter/Backspace/Tab/Delete) and keydown fired at the focused control.

Usage: js_quickjs.py <path/to/netsurf-all-3.11>   (idempotent; applies js_quickjs.diff)"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nsdiff

if len(sys.argv) != 2:
    sys.exit(__doc__)
nsdiff.apply('js_quickjs', sys.argv[1], [
    ('../quickjs/qjs.c', 'netsurf/content/handlers/javascript/quickjs/qjs.c'),
    ('../quickjs/dom.js', 'netsurf/content/handlers/javascript/quickjs/dom.js'),
    ('../quickjs/Makefile', 'netsurf/content/handlers/javascript/quickjs/Makefile'),
])
