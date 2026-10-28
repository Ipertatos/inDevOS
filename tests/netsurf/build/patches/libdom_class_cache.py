#!/usr/bin/env python3
"""inDevOS NetSurf patch (libdom): refresh the class cache when an existing
class attribute is changed.

libdom caches an element's class list when the class attribute is *created*,
but setAttribute('class', ...) / el.className = ... on an element that
already has a class attribute only replaces the value, so CSS selection keeps
matching the old classes (JS class switches never restyle).  Idempotent.
Usage: libdom_class_cache.py <libdom>/src/core/element.c
"""
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
MARK = "inDevOS: refresh class cache"
if MARK in s:
    print("libdom_class_cache: already applied"); sys.exit(0)
old = """		err = dom_attr_set_value(match->attr, value);
		if (err != DOM_NO_ERR)
			return err;

		success = true;
		err = _dom_dispatch_subtree_modified_event(doc,"""
new = """		err = dom_attr_set_value(match->attr, value);
		if (err != DOM_NO_ERR)
			return err;

		/* inDevOS: refresh class cache (only built on attr creation) */
		if (namespace == NULL &&
				dom_string_isequal(name, doc->class_string)) {
			err = _dom_element_create_classes(element,
					dom_string_data(value));
			if (err != DOM_NO_ERR)
				return err;
		}

		success = true;
		err = _dom_dispatch_subtree_modified_event(doc,"""
if s.count(old) != 1:
    sys.exit("libdom_class_cache: anchor not found / not unique in " + p)
s = s.replace(old, new, 1)
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("libdom_class_cache: patched", p)
