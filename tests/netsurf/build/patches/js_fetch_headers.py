#!/usr/bin/env python3
"""Let script fetch()/XMLHttpRequest send their own request headers.

llcache built only the cache-validation and Referer headers, so
fetch(url, {headers: {Accept: 'application/json'}}) went out with curl's
"Accept: */*" and GitHub's React data endpoints (tree-commit-info,
overview-files, ...) answered 400.  The QuickJS fetch sets the global
llcache_extra_headers ("Name: value\\n" lines, forbidden names filtered on
the JS side) around llcache_handle_retrieve(); llcache_object_fetch keeps a
copy in the object's fetch context and every (re)fetch sends it.

Usage: js_fetch_headers.py <path/to/netsurf-all-3.11>   (idempotent; applies js_fetch_headers.diff)"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nsdiff

if len(sys.argv) != 2:
    sys.exit(__doc__)
nsdiff.apply('js_fetch_headers', sys.argv[1], [])
