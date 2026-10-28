#!/usr/bin/env python3
"""Fix leaks of form controls that are outside any <form> ("orphans"): they
are remembered on their DOM node, reused when boxes are rebuilt, freed when
the layout is freed; a reused text control drops its old textarea/initial.

Usage: forms_orphans.py <path/to/netsurf-all-3.11>   (idempotent; applies forms_orphans.diff)"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nsdiff

if len(sys.argv) != 2:
    sys.exit(__doc__)
nsdiff.apply('forms_orphans', sys.argv[1], [])
