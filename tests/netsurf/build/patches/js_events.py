#!/usr/bin/env python3
"""inDevOS NetSurf patch: fire DOMContentLoaded and Window addEventListener() load.

NetSurf 3.11 only calls window.onload / <body onload>.  It never fires
DOMContentLoaded and never runs listeners added with
window.addEventListener('load'|'DOMContentLoaded', fn).  This patch:
  * html.c: fires DOMContentLoaded after all scripts (incl. deferred) ran
  * dukky.c js_fire_event(): DOMContentLoaded is dispatched (bubbling) at the
    Document through libdom, so document.addEventListener() listeners run, and
    then Window listeners run; for load, Window listeners run before onload.
Idempotent.  Usage: js_events.py <netsurf source dir>
"""
import sys, os

root = sys.argv[1]
MARK = "inDevOS: js_events"

def patch(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, encoding="utf-8").read()
    if MARK in s:
        print("js_events: already applied to", rel); return
    for old, new in pairs:
        if old not in s:
            sys.exit("js_events: anchor not found in %s: %r" % (rel, old[:60]))
        s = s.replace(old, new, 1)
    open(p, "w", encoding="utf-8", newline="\n").write(s)
    print("js_events: patched", rel)

patch("content/handlers/html/html.c", [(
"""	/* fire a simple event that bubbles named DOMContentLoaded at
	 * the Document.
	 */
""",
"""	/* fire a simple event that bubbles named DOMContentLoaded at
	 * the Document.
	 */
	/* inDevOS: js_events - actually fire it */
	if (htmlc->jsthread != NULL) {
		js_fire_event(htmlc->jsthread, "DOMContentLoaded",
			      htmlc->document, NULL);
	}
""")])

HELPER = r'''
/* inDevOS: js_events - run listeners added with window.addEventListener(type) */
static void dukky_fire_window_listeners(jsthread *thread, const char *type)
{
	dom_event *evt;
	dom_string *t;
	dom_exception e;
	duk_size_t n, idx;

	if (dom_event_create(&evt) != DOM_NO_ERR)
		return;
	if (dom_string_create((const uint8_t *)type, strlen(type), &t) != DOM_NO_ERR) {
		dom_event_unref(evt);
		return;
	}
	e = dom_event_init(evt, t, false, false);
	dom_string_unref(t);
	if (e != DOM_NO_ERR) {
		dom_event_unref(evt);
		return;
	}
	dukky_enter_thread(thread);
	duk_push_global_object(CTX);
	/* win */
	duk_get_prop_string(CTX, -1, EVENT_LISTENER_JS_MAGIC);
	/* win map */
	if (duk_is_object(CTX, -1)) {
		duk_get_prop_string(CTX, -1, type);
		/* win map arr */
		if (duk_is_array(CTX, -1)) {
			n = duk_get_length(CTX, -1);
			for (idx = 0; idx < n; idx++) {
				duk_get_prop_index(CTX, -1, (duk_uarridx_t)idx);
				/* win map arr ent */
				if (duk_is_object(CTX, -1) &&
				    !duk_has_prop_index(CTX, -1, 2)) {
					duk_get_prop_index(CTX, -1, 0);
					/* win map arr ent cb */
					if (duk_is_callable(CTX, -1)) {
						duk_dup(CTX, -5);
						dukky_push_event(CTX, evt);
						/* win map arr ent cb win ev */
						dukky_reset_start_time(CTX);
						if (duk_pcall_method(CTX, 1) != 0) {
							NSLOG(jserrors, WARNING,
							      "Uncaught error in window '%s' listener: %s",
							      type, duk_safe_to_string(CTX, -1));
						}
					}
					duk_pop(CTX);
					/* win map arr ent */
				}
				duk_pop(CTX);
				/* win map arr */
			}
		}
		duk_pop(CTX);
		/* win map */
	}
	duk_pop_2(CTX);
	js_event_cleanup(thread, evt);
	dom_event_unref(evt);
	dukky_leave_thread(thread);
}

bool js_fire_event(jsthread *thread, const char *type, struct dom_document *doc, struct dom_node *target)
{'''

patch("content/handlers/javascript/duktape/dukky.c", [
("\nbool js_fire_event(jsthread *thread, const char *type, struct dom_document *doc, struct dom_node *target)\n{", HELPER),
("""	if (strcmp(type, "load") != 0)
		/* Swallow non-load events quietly */
		return true;
""",
"""	if (strcmp(type, "DOMContentLoaded") == 0) {
		/* inDevOS: bubbling event at the Document, then Window */
		dom_string *t;
		if (dom_event_create(&evt) != DOM_NO_ERR)
			return true;
		if (dom_string_create((const uint8_t *)type, strlen(type),
				      &t) == DOM_NO_ERR) {
			if (dom_event_init(evt, t, true, false) == DOM_NO_ERR) {
				bool ok;
				dukky_enter_thread(thread);
				dom_event_target_dispatch_event(doc, evt, &ok);
				js_event_cleanup(thread, evt);
				dukky_leave_thread(thread);
			}
			dom_string_unref(t);
		}
		dom_event_unref(evt);
		dukky_fire_window_listeners(thread, type);
		return true;
	}

	if (strcmp(type, "load") != 0)
		/* Swallow non-load events quietly */
		return true;

	/* inDevOS: window.addEventListener('load') listeners */
	dukky_fire_window_listeners(thread, "load");
"""),
])
