#!/usr/bin/env python3
"""Throttle the whole-page reformat that every arriving object triggers.

html_object_callback reformatted the page on CONTENT_MSG_READY for each
reformattable object (SVG) with no rate limit: Wikipedia articles (hundreds
of SVG icons) spent seconds per fetch burst in layout.  Object reformats now
honour content reformat_time (3x the cost of the previous layout, at least
min_reflow_period) and a single deferred reformat covers the skipped ones;
it is cancelled when the content is destroyed.

Usage: html_reflow_throttle.py <path/to/netsurf-all-3.11>   (idempotent; applies html_reflow_throttle.diff)"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nsdiff

if len(sys.argv) != 2:
    sys.exit(__doc__)
nsdiff.apply('html_reflow_throttle', sys.argv[1], [])
