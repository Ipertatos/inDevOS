#!/usr/bin/env python3
"""inDevOS NetSurf patch: honour <link rel="preload" as="style">.

Many sites load CSS asynchronously with
  <link rel="preload" href="x.css" as="style" onload="this.rel='stylesheet'">
  <noscript><link rel="stylesheet" href="x.css"></noscript>
NetSurf 3.11 never fetches preloads and does not react to the rel change, and
with JavaScript enabled it ignores <noscript>, so such pages render unstyled.
Treat rel=preload + as=style as a normal stylesheet link.  Idempotent.
Usage: html_preload_css.py <netsurf>/content/handlers/html/css.c
"""
import sys

path = sys.argv[1]
src = open(path, encoding="utf-8").read()
MARK = "inDevOS: rel=preload as=style"
if MARK in src:
    print("html_preload_css: already applied")
    sys.exit(0)

old = """	if (strcasestr(dom_string_data(rel), "stylesheet") == NULL) {
		dom_string_unref(rel);
		return true;
	} else if"""
new = """	if (strcasestr(dom_string_data(rel), "stylesheet") == NULL) {
		/* inDevOS: rel=preload as=style is the common async-CSS idiom
		 * (onload flips rel to stylesheet, which NetSurf never sees),
		 * so load it as a stylesheet. */
		bool preload_style = false;
		if (strcasestr(dom_string_data(rel), "preload") != NULL) {
			dom_string *as_name = NULL, *as = NULL;
			if (dom_string_create((const uint8_t *)"as", 2,
					&as_name) == DOM_NO_ERR) {
				if (dom_element_get_attribute(node, as_name,
						&as) == DOM_NO_ERR && as != NULL) {
					preload_style =
						dom_string_byte_length(as) == 5 &&
						strncasecmp(dom_string_data(as),
							"style", 5) == 0;
					dom_string_unref(as);
				}
				dom_string_unref(as_name);
			}
		}
		if (!preload_style) {
			dom_string_unref(rel);
			return true;
		}
	} else if"""
if old not in src:
    sys.exit("html_preload_css: anchor not found in " + path)
src = src.replace(old, new, 1)
if "#include <strings.h>" not in src:
    src = src.replace("#include <string.h>", "#include <string.h>\n#include <strings.h>", 1)
open(path, "w", encoding="utf-8", newline="\n").write(src)
print("html_preload_css: patched", path)
