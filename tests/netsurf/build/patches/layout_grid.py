#!/usr/bin/env python3
"""NetSurf CSS Grid layout: display:grid/inline-grid map to the flex box types
and layout_flex dispatches to layout_grid (grid/layout_grid.c); gap in flex
min/max widths, blockification of inline children of flex/grid containers,
inline-flex/inline-grid placed on lines, box_coords tolerates floats laid
out as flex/grid items (no float_container).

Usage: layout_grid.py <path/to/netsurf-all-3.11>   (idempotent; applies layout_grid.diff)"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nsdiff

if len(sys.argv) != 2:
    sys.exit(__doc__)
nsdiff.apply('layout_grid', sys.argv[1], [
    ('grid/layout_grid.c', 'netsurf/content/handlers/html/layout_grid.c'),
])
