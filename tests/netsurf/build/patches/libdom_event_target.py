#!/usr/bin/env python3
"""inDevOS NetSurf patch (libdom): don't run listeners on the event target
twice.

_dom_node_dispatch_event puts the target itself in the capture/bubble path,
so a normal (non-capture) listener on the target ran in the AT_TARGET phase
and again in the BUBBLING phase (every onclick/addEventListener fired twice).
Per DOM, the target is only visited in AT_TARGET.  Idempotent.
Usage: libdom_event_target.py <libdom>/src/core/node.c
"""
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
MARK = "inDevOS: target only in AT_TARGET"
if MARK in s:
    print("libdom_event_target: already applied"); sys.exit(0)
reps = [
("""		dom_node_internal *node =
			(dom_node_internal *) targets[targetnr - 1];

		err = _dom_event_target_dispatch(targets[targetnr - 1],
				&node->eti, evt, DOM_CAPTURING_PHASE, success);""",
"""		dom_node_internal *node =
			(dom_node_internal *) targets[targetnr - 1];

		/* inDevOS: target only in AT_TARGET */
		if (targets[targetnr - 1] == et)
			continue;
		err = _dom_event_target_dispatch(targets[targetnr - 1],
				&node->eti, evt, DOM_CAPTURING_PHASE, success);"""),
("""		dom_node_internal *node =
			(dom_node_internal *) targets[targetnr];
		err = _dom_event_target_dispatch(targets[targetnr],
				&node->eti, evt, DOM_BUBBLING_PHASE, success);""",
"""		dom_node_internal *node =
			(dom_node_internal *) targets[targetnr];
		if (targets[targetnr] == et)
			continue;
		err = _dom_event_target_dispatch(targets[targetnr],
				&node->eti, evt, DOM_BUBBLING_PHASE, success);"""),
]
for old, new in reps:
    if s.count(old) != 1:
        sys.exit("libdom_event_target: anchor not found / not unique in " + p)
    s = s.replace(old, new, 1)
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("libdom_event_target: patched", p)
