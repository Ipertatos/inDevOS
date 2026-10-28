/*
 * QuickJS JavaScript backend for NetSurf (inDevOS port).
 *
 * Implements javascript/js.h on top of QuickJS.  The DOM API itself is
 * written in JavaScript (dom.js, embedded at build time); this file
 * provides a small set of native primitives on a hidden "__ns" object
 * plus heap/thread lifecycle, events, timers, fetch and localStorage.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#include <nsutils/time.h>
#include <dom/dom.h>
#include <libcss/libcss.h>
#include <dom/bindings/hubbub/parser.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/nsoption.h"
#include "utils/corestrings.h"
#include "utils/useragent.h"
#include "netsurf/misc.h"
#include "netsurf/console.h"
#include "netsurf/browser_window.h"
#include "desktop/gui_internal.h"
#include "desktop/browser_private.h"
#include "content/content.h"
#include "content/content_protected.h"
#include "content/llcache.h"
#include "content/urldb.h"
#include "content/handlers/html/private.h"
#include "content/handlers/html/html.h"
#include "content/handlers/html/box.h"
#include "content/handlers/html/box_construct.h"
#include "content/handlers/html/box_inspect.h"
#include "content/handlers/html/form_internal.h"
#include "content/hlcache.h"
#include "netsurf/content.h"
#include "desktop/selection.h"
#include "desktop/textarea.h"
#include "utils/talloc.h"
#include "javascript/js.h"
#include "javascript/content.h"

#include <quickjs.h>
#include <curl/curl.h>

#include "quickjs/dom.js.inc"

/* builds against Bellard QuickJS (2025-04-26) and quickjs-ng */
#ifdef QJS_VERSION_MAJOR
#define QJS_NEW_CLASS_ID(rt, id) JS_NewClassID((rt), (id))
#define QJS_ENGINE_VERSION JS_GetVersion()
#else
#define QJS_NEW_CLASS_ID(rt, id) JS_NewClassID(id)
#define QJS_ENGINE_VERSION "2025-04-26"
#endif
#define QJS_COUNTOF(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define QJS_STACK_SIZE (384 * 1024)

/* ------------------------------------------------------------------ */
/* types                                                               */

struct jsheap {
	JSRuntime *rt;
	int timeout;		/* seconds */
	uint64_t exec_start;	/* ms; 0 = not running */
	unsigned live_threads;
	bool pending_destroy;
};

struct qjs_listener_pw {
	struct jsthread *t;
	bool capture;
};

struct qjs_reg {		/* libdom listener registrations */
	dom_node *node;
	dom_string *type;
};

struct qjs_nodemap_ent {
	dom_node *node;
	JSValue obj;
};

struct qjs_evmap_ent {
	dom_event *evt;
	JSValue obj;
};

struct qjs_timer {
	struct jsthread *t;
	int id;
	int ms;
	bool repeat;
	struct qjs_timer *next;
};

struct qjs_fetch {
	struct jsthread *t;
	int id;
	llcache_handle *h;
	char *post;
	bool done;
	struct qjs_fetch *next;
};

struct jsthread {
	jsheap *heap;
	JSContext *ctx;
	bool pending_destroy;
	bool closed;
	unsigned in_use;
	struct browser_window *bw;
	html_content *htmlc;

	JSValue fn_proto;	/* (nodeType, tag) -> prototype */
	JSValue fn_dispatch;	/* (currentTarget, event, phase) */
	JSValue fn_window;	/* (type, event|undefined) */
	JSValue fn_timer;	/* (id) */
	JSValue fn_fetchdone;	/* (id, ok, status, url, body, ctype, err) */
	JSValue fn_evproto;	/* (type) -> prototype */

	struct qjs_nodemap_ent *nodes;
	size_t nodes_cap, nodes_cnt;

	struct qjs_evmap_ent *evs;
	size_t evs_cap, evs_cnt;

	struct qjs_reg *regs;
	size_t regs_cap, regs_cnt;

	struct qjs_listener_pw pw_cap, pw_bub;
	dom_event_listener *l_cap, *l_bub;

	struct qjs_timer *timers;
	struct qjs_fetch *fetches;
};

static JSClassID qjs_node_class_id;
static JSClassID qjs_event_class_id;

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static jsthread *T(JSContext *ctx)
{
	return JS_GetContextOpaque(ctx);
}

static void qjs_run_jobs(jsthread *t);
static char *qjs_mod_normalize(JSContext *ctx, const char *base, const char *name, void *opaque);
static JSModuleDef *qjs_mod_load(JSContext *ctx, const char *name, void *opaque);

static void qjs_log_exception(jsthread *t, const char *where)
{
	JSContext *ctx = t->ctx;
	JSValue ex = JS_GetException(ctx);
	const char *msg = JS_ToCString(ctx, ex);
	const char *stack = NULL;
	JSValue st = JS_UNDEFINED;
	char buf[2048];
	int len;

	if (JS_IsError(ctx, ex)) {
		st = JS_GetPropertyStr(ctx, ex, "stack");
		if (!JS_IsUndefined(st))
			stack = JS_ToCString(ctx, st);
	}
	len = snprintf(buf, sizeof(buf), "%s: %s%s%s", where ? where : "js",
		       msg ? msg : "(exception)",
		       stack ? "\n" : "", stack ? stack : "");
	if (len < 0) len = 0;
	if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
	NSLOG(netsurf, INFO, "QJS error %.*s", len, buf);
	fprintf(stderr, "[qjs] %.*s\n", len, buf);
	if (t->bw != NULL && !t->closed) {
		browser_window_console_log(t->bw, BW_CS_SCRIPT_ERROR,
				buf, len, BW_CS_FLAG_LEVEL_ERROR);
	}
	if (stack) JS_FreeCString(ctx, stack);
	JS_FreeValue(ctx, st);
	if (msg) JS_FreeCString(ctx, msg);
	JS_FreeValue(ctx, ex);
}

static void qjs_enter(jsthread *t)
{
	if (t->in_use == 0) {
		nsu_getmonotonic_ms(&t->heap->exec_start);
	}
	t->in_use++;
}

static void qjs_leave(jsthread *t);

static int qjs_interrupt(JSRuntime *rt, void *opaque)
{
	jsheap *heap = opaque;
	uint64_t now;
	if (heap->exec_start == 0 || heap->timeout <= 0)
		return 0;
	nsu_getmonotonic_ms(&now);
	if (now - heap->exec_start > (uint64_t)heap->timeout * 1000) {
		NSLOG(netsurf, WARNING, "QJS script timeout");
		return 1;
	}
	return 0;
}

/* call a JS function; takes no ownership of args; returns true on success */
static bool qjs_call(jsthread *t, JSValueConst fn, JSValueConst this_obj,
		     int argc, JSValueConst *argv, const char *where)
{
	JSValue r;
	bool ok = true;
	if (!JS_IsFunction(t->ctx, fn))
		return false;
	qjs_enter(t);
	r = JS_Call(t->ctx, fn, this_obj, argc, argv);
	if (JS_IsException(r)) {
		qjs_log_exception(t, where);
		ok = false;
	}
	JS_FreeValue(t->ctx, r);
	qjs_run_jobs(t);
	qjs_leave(t);
	return ok;
}

static JSValue qjs_dstr(JSContext *ctx, dom_string *s)
{
	JSValue v;
	if (s == NULL)
		return JS_NULL;
	v = JS_NewStringLen(ctx, dom_string_data(s), dom_string_byte_length(s));
	dom_string_unref(s);
	return v;
}

static dom_string *qjs_todstr(JSContext *ctx, JSValueConst v)
{
	size_t len;
	const char *s = JS_ToCStringLen(ctx, &len, v);
	dom_string *ds = NULL;
	if (s == NULL)
		return NULL;
	if (dom_string_create((const uint8_t *)s, len, &ds) != DOM_NO_ERR)
		ds = NULL;
	JS_FreeCString(ctx, s);
	return ds;
}

/* ------------------------------------------------------------------ */
/* node wrappers                                                        */

static void qjs_node_finalizer(JSRuntime *rt, JSValue val)
{
	dom_node *n = JS_GetOpaque(val, qjs_node_class_id);
	if (n != NULL)
		dom_node_unref(n);
}

static JSClassDef qjs_node_class = {
	"NSNode",
	.finalizer = qjs_node_finalizer,
};

static void qjs_event_finalizer(JSRuntime *rt, JSValue val)
{
	dom_event *e = JS_GetOpaque(val, qjs_event_class_id);
	if (e != NULL)
		dom_event_unref(e);
}

static JSClassDef qjs_event_class = {
	"NSEvent",
	.finalizer = qjs_event_finalizer,
};

static size_t qjs_hash_ptr(const void *p, size_t cap)
{
	uintptr_t x = (uintptr_t)p;
	x ^= x >> 17;
	x *= 0x9E3779B97F4A7C15ull;
	x ^= x >> 29;
	return (size_t)x & (cap - 1);
}

static JSValue *qjs_nodemap_find(jsthread *t, dom_node *n)
{
	size_t i;
	if (t->nodes_cap == 0)
		return NULL;
	i = qjs_hash_ptr(n, t->nodes_cap);
	while (t->nodes[i].node != NULL) {
		if (t->nodes[i].node == n)
			return &t->nodes[i].obj;
		i = (i + 1) & (t->nodes_cap - 1);
	}
	return NULL;
}

static bool qjs_nodemap_insert(jsthread *t, dom_node *n, JSValue obj)
{
	size_t i;
	if ((t->nodes_cnt + 1) * 2 > t->nodes_cap) {
		size_t ncap = t->nodes_cap ? t->nodes_cap * 2 : 256;
		struct qjs_nodemap_ent *nt = calloc(ncap, sizeof(*nt));
		size_t k;
		if (nt == NULL)
			return false;
		for (k = 0; k < t->nodes_cap; k++) {
			if (t->nodes[k].node != NULL) {
				size_t j = qjs_hash_ptr(t->nodes[k].node, ncap);
				while (nt[j].node != NULL)
					j = (j + 1) & (ncap - 1);
				nt[j] = t->nodes[k];
			}
		}
		free(t->nodes);
		t->nodes = nt;
		t->nodes_cap = ncap;
	}
	i = qjs_hash_ptr(n, t->nodes_cap);
	while (t->nodes[i].node != NULL)
		i = (i + 1) & (t->nodes_cap - 1);
	t->nodes[i].node = n;
	t->nodes[i].obj = obj;
	t->nodes_cnt++;
	return true;
}

static JSValue qjs_wrap_node(jsthread *t, dom_node *n)
{
	JSContext *ctx = t->ctx;
	JSValue *cached, proto, obj, argv[2];
	dom_node_type type = 0;
	dom_string *name = NULL;

	if (n == NULL)
		return JS_NULL;
	cached = qjs_nodemap_find(t, n);
	if (cached != NULL)
		return JS_DupValue(ctx, *cached);

	dom_node_get_node_type(n, &type);
	argv[0] = JS_NewInt32(ctx, type);
	if (type == DOM_ELEMENT_NODE &&
	    dom_node_get_node_name(n, &name) == DOM_NO_ERR && name != NULL) {
		dom_string *lc = NULL;
		if (dom_string_tolower(name, true, &lc) == DOM_NO_ERR) {
			dom_string_unref(name);
			name = lc;
		}
		argv[1] = qjs_dstr(ctx, name);
	} else {
		argv[1] = JS_NULL;
	}
	proto = JS_Call(ctx, t->fn_proto, JS_UNDEFINED, 2, argv);
	JS_FreeValue(ctx, argv[1]);
	if (JS_IsException(proto)) {
		qjs_log_exception(t, "proto");
		proto = JS_NULL;
	}
	obj = JS_NewObjectProtoClass(ctx, proto, qjs_node_class_id);
	JS_FreeValue(ctx, proto);
	if (JS_IsException(obj))
		return obj;
	JS_SetOpaque(obj, dom_node_ref(n));
	qjs_nodemap_insert(t, n, JS_DupValue(ctx, obj));
	return obj;
}

static dom_node *qjs_node(JSContext *ctx, JSValueConst v)
{
	return JS_GetOpaque(v, qjs_node_class_id);
}

#define NODE_ARG(var, idx) \
	dom_node *var = (argc > (idx)) ? qjs_node(ctx, argv[idx]) : NULL; \
	if (var == NULL) return JS_ThrowTypeError(ctx, "not a node")

/* ------------------------------------------------------------------ */
/* event wrappers                                                       */

static JSValue qjs_wrap_event(jsthread *t, dom_event *e)
{
	JSContext *ctx = t->ctx;
	JSValue proto, obj, info, arg;
	dom_string *type = NULL;
	bool b;
	size_t i;

	for (i = 0; i < t->evs_cnt; i++) {
		if (t->evs[i].evt == e)
			return JS_DupValue(ctx, t->evs[i].obj);
	}

	dom_event_get_type(e, &type);
	arg = type ? JS_NewStringLen(ctx, dom_string_data(type),
				     dom_string_byte_length(type))
		   : JS_NewString(ctx, "");
	proto = JS_Call(ctx, t->fn_evproto, JS_UNDEFINED, 1, &arg);
	if (JS_IsException(proto)) {
		qjs_log_exception(t, "evproto");
		proto = JS_NULL;
	}
	obj = JS_NewObjectProtoClass(ctx, proto, qjs_event_class_id);
	JS_FreeValue(ctx, proto);
	if (JS_IsException(obj)) {
		JS_FreeValue(ctx, arg);
		if (type) dom_string_unref(type);
		return obj;
	}
	dom_event_ref(e);
	JS_SetOpaque(obj, e);

	info = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, info, "type", arg);
	b = false; dom_event_get_bubbles(e, &b);
	JS_SetPropertyStr(ctx, info, "bubbles", JS_NewBool(ctx, b));
	b = false; dom_event_get_cancelable(e, &b);
	JS_SetPropertyStr(ctx, info, "cancelable", JS_NewBool(ctx, b));
	b = false; dom_event_get_is_trusted(e, &b);
	JS_SetPropertyStr(ctx, info, "isTrusted", JS_NewBool(ctx, b));

	if (type != NULL &&
	    (dom_string_isequal(type, corestring_dom_keydown) ||
	     strncmp(dom_string_data(type), "key", 3) == 0)) {
		dom_string *key = NULL;
		if (dom_keyboard_event_get_key((dom_keyboard_event *)e, &key)
		    == DOM_NO_ERR && key != NULL) {
			JS_SetPropertyStr(ctx, info, "key", qjs_dstr(ctx, key));
		}
		b = false; dom_keyboard_event_get_ctrl_key((dom_keyboard_event *)e, &b);
		JS_SetPropertyStr(ctx, info, "ctrlKey", JS_NewBool(ctx, b));
		b = false; dom_keyboard_event_get_shift_key((dom_keyboard_event *)e, &b);
		JS_SetPropertyStr(ctx, info, "shiftKey", JS_NewBool(ctx, b));
		b = false; dom_keyboard_event_get_alt_key((dom_keyboard_event *)e, &b);
		JS_SetPropertyStr(ctx, info, "altKey", JS_NewBool(ctx, b));
		b = false; dom_keyboard_event_get_meta_key((dom_keyboard_event *)e, &b);
		JS_SetPropertyStr(ctx, info, "metaKey", JS_NewBool(ctx, b));
	}
	if (type) dom_string_unref(type);
	JS_DefinePropertyValueStr(ctx, obj, "__ni", info, 0);

	if (t->evs_cnt == t->evs_cap) {
		size_t ncap = t->evs_cap ? t->evs_cap * 2 : 8;
		void *n = realloc(t->evs, ncap * sizeof(*t->evs));
		if (n == NULL)
			return obj;
		t->evs = n;
		t->evs_cap = ncap;
	}
	t->evs[t->evs_cnt].evt = e;
	t->evs[t->evs_cnt].obj = JS_DupValue(ctx, obj);
	t->evs_cnt++;
	return obj;
}

static dom_event *qjs_event(JSContext *ctx, JSValueConst v)
{
	return JS_GetOpaque(v, qjs_event_class_id);
}

/* ------------------------------------------------------------------ */
/* libdom listeners                                                     */

static void qjs_generic_handler(struct dom_event *evt, void *pw)
{
	struct qjs_listener_pw *lpw = pw;
	jsthread *t = lpw->t;
	dom_event_flow_phase phase = 0;
	dom_event_target *cur = NULL;
	JSValue argv[3];

	if (t == NULL || t->closed || t->pending_destroy)
		return;
	dom_event_get_event_phase(evt, &phase);
	if (lpw->capture) {
		if (phase == DOM_BUBBLING_PHASE)
			return;
	} else {
		if (phase != DOM_BUBBLING_PHASE)
			return;
	}
	if (dom_event_get_current_target(evt, &cur) != DOM_NO_ERR || cur == NULL)
		return;

	argv[0] = qjs_wrap_node(t, (dom_node *)cur);
	dom_node_unref((dom_node *)cur);
	argv[1] = qjs_wrap_event(t, evt);
	argv[2] = JS_NewInt32(t->ctx, phase);
	qjs_call(t, t->fn_dispatch, JS_UNDEFINED, 3, argv, "event");
	JS_FreeValue(t->ctx, argv[0]);
	JS_FreeValue(t->ctx, argv[1]);
}

/* register libdom capture+bubble listeners for (node, type) once */
static bool qjs_listen(jsthread *t, dom_node *node, dom_string *type)
{
	size_t i;
	for (i = 0; i < t->regs_cnt; i++) {
		if (t->regs[i].node == node &&
		    dom_string_isequal(t->regs[i].type, type))
			return true;
	}
	if (t->regs_cnt == t->regs_cap) {
		size_t ncap = t->regs_cap ? t->regs_cap * 2 : 64;
		void *n = realloc(t->regs, ncap * sizeof(*t->regs));
		if (n == NULL)
			return false;
		t->regs = n;
		t->regs_cap = ncap;
	}
	if (dom_event_target_add_event_listener(node, type, t->l_cap, true)
	    != DOM_NO_ERR)
		return false;
	dom_event_target_add_event_listener(node, type, t->l_bub, false);
	t->regs[t->regs_cnt].node = dom_node_ref(node);
	t->regs[t->regs_cnt].type = dom_string_ref(type);
	t->regs_cnt++;
	return true;
}

static void qjs_unlisten_all(jsthread *t)
{
	size_t i;
	for (i = 0; i < t->regs_cnt; i++) {
		dom_event_target_remove_event_listener(t->regs[i].node,
				t->regs[i].type, t->l_cap, true);
		dom_event_target_remove_event_listener(t->regs[i].node,
				t->regs[i].type, t->l_bub, false);
		dom_string_unref(t->regs[i].type);
		dom_node_unref(t->regs[i].node);
	}
	free(t->regs);
	t->regs = NULL;
	t->regs_cnt = t->regs_cap = 0;
}

/* ------------------------------------------------------------------ */
/* natives: node tree                                                   */

static JSValue n_nav(JSContext *ctx, JSValueConst this_val, int argc,
		     JSValueConst *argv, int which)
{
	dom_node *r = NULL;
	dom_exception exc = DOM_NO_ERR;
	JSValue v;
	NODE_ARG(n, 0);
	switch (which) {
	case 0: exc = dom_node_get_parent_node(n, &r); break;
	case 1: exc = dom_node_get_first_child(n, &r); break;
	case 2: exc = dom_node_get_last_child(n, &r); break;
	case 3: exc = dom_node_get_next_sibling(n, &r); break;
	case 4: exc = dom_node_get_previous_sibling(n, &r); break;
	case 5: exc = dom_node_get_owner_document(n, (dom_document **)&r); break;
	}
	if (exc != DOM_NO_ERR || r == NULL)
		return JS_NULL;
	v = qjs_wrap_node(T(ctx), r);
	dom_node_unref(r);
	return v;
}

static JSValue n_info(JSContext *ctx, JSValueConst this_val, int argc,
		      JSValueConst *argv, int which)
{
	dom_string *s = NULL;
	NODE_ARG(n, 0);
	switch (which) {
	case 0: dom_node_get_node_name(n, &s); break;
	case 1: dom_node_get_node_value(n, &s); break;
	case 2: dom_node_get_text_content(n, &s); break;
	}
	return qjs_dstr(ctx, s);
}

/* ------------------------------------------------------------------ */
/* Post-layout DOM changes: rebuild the box tree ("rebox").
 *
 * NetSurf's layout is static once built.  When script mutates the
 * connected DOM after the box tree exists we rebuild the whole box tree
 * with dom_to_box() into a fresh talloc context, then swap it in,
 * re-point form gadgets/focus at the new boxes, drop the old box-bound
 * objects and reformat.  Debounced and rate limited by the cost of the
 * previous rebox.
 */
struct qjs_rebox {
	struct qjs_rebox *next;
	html_content *c;
	bool scheduled, running, dirty, closed;
	void *old_bctx;
	struct box *old_layout;
	struct content_html_object *old_objs;
	uint64_t t_start, t_end, cost;
	unsigned count;
};
static struct qjs_rebox *qjs_reboxes;

static struct qjs_rebox *qjs_rebox_find(html_content *c)
{
	struct qjs_rebox *r;
	for (r = qjs_reboxes; r != NULL; r = r->next)
		if (r->c == c)
			return r;
	return NULL;
}

static void qjs_rebox_unlink(struct qjs_rebox *st)
{
	struct qjs_rebox **pp;
	for (pp = &qjs_reboxes; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == st) {
			*pp = st->next;
			break;
		}
	}
	free(st);
}

static void qjs_rebox_cb(void *p);

static void qjs_rebox_schedule(struct qjs_rebox *st)
{
	uint64_t now, interval, delay = 20;
	if (st->scheduled || st->running || st->closed)
		return;
	nsu_getmonotonic_ms(&now);
	interval = st->cost * 3;
	if (interval < 150)
		interval = 150;
	if (st->t_end != 0 && now < st->t_end + interval)
		delay = st->t_end + interval - now;
	if (delay < 20)
		delay = 20;
	st->scheduled = true;
	guit->misc->schedule((int)delay, qjs_rebox_cb, st);
}

static bool qjs_connected(html_content *c, dom_node *n)
{
	dom_node *cur = n, *p;
	if (n == NULL || c->document == NULL)
		return false;
	dom_node_ref(cur);
	while (cur != NULL) {
		if (cur == (dom_node *)c->document) {
			dom_node_unref(cur);
			return true;
		}
		p = NULL;
		dom_node_get_parent_node(cur, &p);
		dom_node_unref(cur);
		cur = p;
	}
	return false;
}

static void qjs_mark_dirty(JSContext *ctx, dom_node *n)
{
	jsthread *t = T(ctx);
	html_content *c;
	struct qjs_rebox *st;
	if (t == NULL || t->closed || (c = t->htmlc) == NULL)
		return;
	if (getenv("NETSURF_QJS_NOREBOX") != NULL)
		return;
	if (c->layout == NULL && c->box_conversion_context == NULL)
		return; /* initial conversion has not started: it will see it */
	if (!qjs_connected(c, n))
		return;
	st = qjs_rebox_find(c);
	if (st == NULL) {
		st = calloc(1, sizeof(*st));
		if (st == NULL)
			return;
		st->c = c;
		st->next = qjs_reboxes;
		qjs_reboxes = st;
	}
	st->dirty = true;
	qjs_rebox_schedule(st);
}

/* iterate a box tree (pre-order) */
static struct box *qjs_box_next(struct box *b, struct box *root)
{
	if (b->children != NULL)
		return b->children;
	while (b != NULL && b != root && b->next == NULL)
		b = b->parent;
	if (b == NULL || b == root)
		return NULL;
	return b->next;
}

static void qjs_rebox_detach_old(struct box *root)
{
	struct box *b, *bb;
	for (b = root; b != NULL; b = qjs_box_next(b, root)) {
		int k;
		for (k = 0, bb = b; k < 2 && bb != NULL;
		     k++, bb = b->list_marker) {
			if (bb->node != NULL) {
				void *ud = NULL;
				if (dom_node_get_user_data(bb->node,
					corestring_dom___ns_key_box_node_data,
					&ud) == DOM_NO_ERR && ud == bb) {
					void *old = NULL;
					dom_node_set_user_data(bb->node,
					  corestring_dom___ns_key_box_node_data,
					  NULL, NULL, &old);
				}
			}
			if (bb->gadget != NULL && bb->gadget->box == bb)
				bb->gadget->box = NULL;
		}
	}
}

static struct box *qjs_rebox_remap(struct box *ob)
{
	struct box *nb;
	if (ob == NULL || ob->node == NULL)
		return NULL;
	nb = box_for_node(ob->node);
	if (nb == NULL || nb->gadget == NULL)
		return NULL;
	return nb;
}

static void qjs_rebox_done(html_content *c, bool success)
{
	struct qjs_rebox *st = qjs_rebox_find(c);
	struct content_html_object **pp, *o, *keep = NULL, **kt = &keep;
	struct box *ofocus = NULL, *nfocus = NULL;
	bool newgadget = false;
	uint64_t now;

	c->box_conversion_context = NULL;
	if (st == NULL) {
		NSLOG(netsurf, ERROR, "QJS rebox done without state");
		return;
	}
	st->running = false;
	if (!success) {
		/* keep the partial tree allocated (node data may point at
		 * it); restore the old context */
		NSLOG(netsurf, ERROR, "QJS rebox failed");
		c->bctx = st->old_bctx;
		c->layout = st->old_layout;
		goto out;
	}

	/* old boxes must not be reachable from nodes or gadgets */
	qjs_rebox_detach_old(st->old_layout);

	/* focus / selection owned by an old textarea box */
	if (c->focus_type == HTML_FOCUS_TEXTAREA) {
		ofocus = c->focus_owner.textarea;
		nfocus = qjs_rebox_remap(ofocus);
		if (nfocus != NULL) {
			newgadget = (nfocus->gadget != ofocus->gadget);
			c->focus_owner.textarea = nfocus;
		} else {
			c->focus_type = HTML_FOCUS_SELF;
			c->focus_owner.self = true;
		}
	} else if (c->focus_type == HTML_FOCUS_CONTENT) {
		c->focus_type = HTML_FOCUS_SELF;
		c->focus_owner.self = true;
	}
	if (c->selection_type == HTML_SELECTION_TEXTAREA) {
		struct box *ns = qjs_rebox_remap(c->selection_owner.textarea);
		if (ns != NULL && ns->gadget == c->selection_owner.textarea->gadget) {
			c->selection_owner.textarea = ns;
		} else {
			c->selection_type = HTML_SELECTION_NONE;
			c->selection_owner.none = true;
		}
	} else if (c->selection_type == HTML_SELECTION_CONTENT) {
		c->selection_type = HTML_SELECTION_NONE;
		c->selection_owner.none = true;
	}
	if (c->sel != NULL && selection_active(c->sel))
		selection_clear(c->sel, false);

	/* objects: new ones are before old_objs; old box-bound ones go */
	pp = &c->object_list;
	while (*pp != NULL && *pp != st->old_objs)
		pp = &(*pp)->next;
	o = *pp;
	*pp = NULL;
	while (o != NULL) {
		struct content_html_object *nx = o->next;
		if (o->box == NULL) {
			o->next = NULL;
			*kt = o;
			kt = &o->next;
		} else {
			if (o->content != NULL) {
				if (content_get_status(o->content) !=
				    CONTENT_STATUS_DONE &&
				    content_get_status(o->content) !=
				    CONTENT_STATUS_ERROR) {
					if (c->base.active > 0)
						c->base.active--;
					hlcache_handle_abort(o->content);
				}
				hlcache_handle_release(o->content);
			}
			if (c->num_objects > 0)
				c->num_objects--;
			free(o);
		}
		o = nx;
	}
	*pp = keep;

	/* controls outside any form whose node lost its box */
	html_forms_free_orphans(st->old_layout, true);
	talloc_free(st->old_bctx);
	st->old_bctx = NULL;
	st->old_layout = NULL;

	if (!c->base.locked && (c->base.status == CONTENT_STATUS_READY ||
				c->base.status == CONTENT_STATUS_DONE)) {
		content__reformat(&c->base, false, c->base.available_width,
				  c->base.available_height);
	}
	/* the control's textarea is recreated with every box: restore the
	 * caret even when the control itself was reused */
	(void)newgadget;
	if (nfocus != NULL && nfocus->gadget != NULL &&
	    (nfocus->gadget->type == GADGET_TEXTBOX ||
	     nfocus->gadget->type == GADGET_PASSWORD ||
	     nfocus->gadget->type == GADGET_TEXTAREA) &&
	    nfocus->gadget->data.text.ta != NULL) {
		textarea_set_caret(nfocus->gadget->data.text.ta, 0x7fffffff);
	}
out:
	nsu_getmonotonic_ms(&now);
	st->t_end = now;
	st->cost = now - st->t_start;
	st->count++;
	NSLOG(netsurf, INFO, "QJS rebox #%u %p %s in %u ms", st->count, c,
	      success ? "ok" : "FAILED", (unsigned)st->cost);
	if (getenv("NETSURF_QJS_DUMP") != NULL) {
		FILE *bf;
		fprintf(stderr, "[qjs] rebox #%u %s in %u ms\n", st->count,
			success ? "ok" : "FAILED", (unsigned)st->cost);
		if (success && c->layout != NULL &&
		    (bf = fopen("/tmp/qjs_boxdump.txt", "w")) != NULL) {
			box_dump(bf, c->layout, 0, false);
			fclose(bf);
		}
	}
	if (st->closed) {
		qjs_rebox_unlink(st);
		return;
	}
	if (st->dirty)
		qjs_rebox_schedule(st);
}

/* drop libcss's per-node selection cache so the rebuild restyles */
static void qjs_css_reset(dom_node *root)
{
	static css_select_handler dummy = {
		.handler_version = CSS_SELECT_HANDLER_VERSION_1,
	};
	dom_node *n = root, *nx;
	dom_node_ref(n);
	while (n != NULL) {
		void *data = NULL, *old = NULL;
		if (dom_node_get_user_data(n,
				corestring_dom___ns_key_libcss_node_data,
				&data) == DOM_NO_ERR && data != NULL) {
			dom_node_set_user_data(n,
				corestring_dom___ns_key_libcss_node_data,
				NULL, NULL, &old);
			if (old != NULL)
				css_libcss_node_data_handler(&dummy,
					CSS_NODE_DELETED, NULL, n, NULL, old);
		}
		/* pre-order walk */
		nx = NULL;
		dom_node_get_first_child(n, &nx);
		if (nx != NULL) {
			dom_node_unref(n);
			n = nx;
			continue;
		}
		while (n != NULL) {
			if (n == root) {
				dom_node_unref(n);
				n = NULL;
				break;
			}
			nx = NULL;
			dom_node_get_next_sibling(n, &nx);
			if (nx != NULL) {
				dom_node_unref(n);
				n = nx;
				break;
			}
			nx = NULL;
			dom_node_get_parent_node(n, &nx);
			dom_node_unref(n);
			n = nx;
		}
	}
}

static void qjs_rebox_cb(void *p)
{
	struct qjs_rebox *st = p;
	html_content *c = st->c;
	struct content_html_object *o;
	dom_node *html = NULL;
	nserror err;

	st->scheduled = false;
	if (st->closed) {
		qjs_rebox_unlink(st);
		return;
	}
	if (!st->dirty)
		return;
	if (c->iframe != NULL || c->frameset != NULL || c->aborted) {
		st->dirty = false; /* unsupported: keep the static layout */
		return;
	}
	for (o = c->object_list; o != NULL; o = o->next) {
		if (o->content != NULL &&
		    content_get_type(o->content) == CONTENT_HTML) {
			st->dirty = false;
			return;
		}
	}
	if ((c->base.status != CONTENT_STATUS_READY &&
	     c->base.status != CONTENT_STATUS_DONE) ||
	    c->layout == NULL || c->box_conversion_context != NULL ||
	    c->base.locked || c->drag_type != HTML_DRAG_NONE ||
	    c->visible_select_menu != NULL ||
	    c->base.textsearch.context != NULL) {
		st->scheduled = true;
		guit->misc->schedule(100, qjs_rebox_cb, st);
		return;
	}
	if (dom_document_get_document_element(c->document, (void *)&html)
	    != DOM_NO_ERR || html == NULL)
		return;

	st->dirty = false;
	nsu_getmonotonic_ms(&st->t_start);
	qjs_css_reset(html);
	st->old_bctx = c->bctx;
	st->old_layout = c->layout;
	st->old_objs = c->object_list;
	c->bctx = NULL;
	st->running = true;
	err = dom_to_box(html, c, qjs_rebox_done, &c->box_conversion_context);
	dom_node_unref(html);
	if (err != NSERROR_OK) {
		NSLOG(netsurf, ERROR, "QJS rebox: dom_to_box failed %d", err);
		st->running = false;
		if (c->bctx != NULL)
			talloc_free(c->bctx);
		c->bctx = st->old_bctx;
		st->old_bctx = NULL;
	}
}

/* html_destroy: a running conversion has been cancelled, so
 * qjs_rebox_done will never run; free the old layout it replaced */
static void qjs_rebox_abandon(html_content *c)
{
	struct qjs_rebox *st = qjs_rebox_find(c);
	if (st == NULL)
		return;
	if (st->scheduled) {
		guit->misc->schedule(-1, qjs_rebox_cb, st);
		st->scheduled = false;
	}
	if (st->running && st->old_bctx != NULL) {
		NSLOG(netsurf, INFO, "QJS rebox %p: content destroyed mid-rebuild",
		      c);
		if (st->old_layout != NULL)
			html_forms_free_orphans(st->old_layout, false);
		if (c->layout == st->old_layout)
			c->layout = NULL;
		talloc_free(st->old_bctx);
		st->old_bctx = NULL;
		st->old_layout = NULL;
	}
	qjs_rebox_unlink(st);
}

/* called when the thread goes away */
static void qjs_rebox_close(html_content *c)
{
	struct qjs_rebox *st = qjs_rebox_find(c);
	if (st == NULL)
		return;
	if (st->scheduled) {
		guit->misc->schedule(-1, qjs_rebox_cb, st);
		st->scheduled = false;
	}
	if (st->running)
		st->closed = true; /* freed when the conversion completes */
	else
		qjs_rebox_unlink(st);
}

static JSValue n_set_text(JSContext *ctx, JSValueConst this_val, int argc,
			  JSValueConst *argv, int which)
{
	dom_string *s;
	dom_exception exc;
	NODE_ARG(n, 0);
	s = qjs_todstr(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
	if (s == NULL)
		return JS_EXCEPTION;
	if (which == 0)
		exc = dom_node_set_text_content(n, s);
	else
		exc = dom_node_set_node_value(n, s);
	dom_string_unref(s);
	if (exc != DOM_NO_ERR)
		return JS_ThrowInternalError(ctx, "DOM error %d", exc);
	qjs_mark_dirty(ctx, n);
	return JS_UNDEFINED;
}

static JSValue n_type(JSContext *ctx, JSValueConst this_val, int argc,
		      JSValueConst *argv)
{
	dom_node_type type = 0;
	NODE_ARG(n, 0);
	dom_node_get_node_type(n, &type);
	return JS_NewInt32(ctx, type);
}

static JSValue n_attr(JSContext *ctx, JSValueConst this_val, int argc,
		      JSValueConst *argv, int which)
{
	dom_string *name, *val = NULL;
	dom_exception exc = DOM_NO_ERR;
	bool has = false;
	JSValue r = JS_UNDEFINED;
	NODE_ARG(n, 0);
	name = qjs_todstr(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
	if (name == NULL)
		return JS_EXCEPTION;
	switch (which) {
	case 0: /* get */
		exc = dom_element_get_attribute(n, name, &val);
		r = (exc == DOM_NO_ERR) ? qjs_dstr(ctx, val) : JS_NULL;
		break;
	case 1: /* set */
		val = qjs_todstr(ctx, argc > 2 ? argv[2] : JS_UNDEFINED);
		if (val == NULL) {
			dom_string_unref(name);
			return JS_EXCEPTION;
		}
		exc = dom_element_set_attribute(n, name, val);
		dom_string_unref(val);
		if (exc != DOM_NO_ERR)
			r = JS_ThrowInternalError(ctx, "setAttribute %d", exc);
		else
			qjs_mark_dirty(ctx, n);
		break;
	case 2: /* remove */
		dom_element_remove_attribute(n, name);
		qjs_mark_dirty(ctx, n);
		break;
	case 3: /* has */
		dom_element_has_attribute(n, name, &has);
		r = JS_NewBool(ctx, has);
		break;
	}
	dom_string_unref(name);
	return r;
}

static JSValue n_attrs(JSContext *ctx, JSValueConst this_val, int argc,
		       JSValueConst *argv)
{
	dom_namednodemap *map = NULL;
	dom_ulong len = 0, i;
	JSValue arr;
	NODE_ARG(n, 0);
	arr = JS_NewArray(ctx);
	if (dom_node_get_attributes(n, &map) != DOM_NO_ERR || map == NULL)
		return arr;
	dom_namednodemap_get_length(map, &len);
	for (i = 0; i < len; i++) {
		dom_node *a = NULL;
		dom_string *nm = NULL, *vl = NULL;
		JSValue pair;
		if (dom_namednodemap_item(map, i, &a) != DOM_NO_ERR || a == NULL)
			continue;
		dom_attr_get_name((dom_attr *)a, &nm);
		dom_attr_get_value((dom_attr *)a, &vl);
		pair = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, pair, 0, qjs_dstr(ctx, nm));
		JS_SetPropertyUint32(ctx, pair, 1, vl ? qjs_dstr(ctx, vl)
						     : JS_NewString(ctx, ""));
		JS_SetPropertyUint32(ctx, arr, i, pair);
		dom_node_unref(a);
	}
	dom_namednodemap_unref(map);
	return arr;
}

static JSValue n_mutate(JSContext *ctx, JSValueConst this_val, int argc,
			JSValueConst *argv, int which)
{
	dom_node *r = NULL;
	dom_exception exc = DOM_NO_ERR;
	dom_node *ref = NULL;
	JSValue v;
	NODE_ARG(p, 0);
	NODE_ARG(c, 1);
	if (argc > 2)
		ref = qjs_node(ctx, argv[2]);
	switch (which) {
	case 0: exc = dom_node_append_child(p, c, &r); break;
	case 1: exc = dom_node_insert_before(p, c, ref, &r); break;
	case 2: exc = dom_node_remove_child(p, c, &r); break;
	case 3:
		if (ref == NULL)
			return JS_ThrowTypeError(ctx, "not a node");
		exc = dom_node_replace_child(p, c, ref, &r);
		break;
	}
	if (exc != DOM_NO_ERR)
		return JS_ThrowInternalError(ctx, "DOM mutation error %d", exc);
	qjs_mark_dirty(ctx, p);
	if (r == NULL)
		return JS_NULL;
	v = qjs_wrap_node(T(ctx), r);
	dom_node_unref(r);
	return v;
}

static JSValue n_clone(JSContext *ctx, JSValueConst this_val, int argc,
		       JSValueConst *argv)
{
	dom_node *r = NULL;
	JSValue v;
	NODE_ARG(n, 0);
	if (dom_node_clone_node(n, argc > 1 && JS_ToBool(ctx, argv[1]), &r)
	    != DOM_NO_ERR || r == NULL)
		return JS_NULL;
	v = qjs_wrap_node(T(ctx), r);
	dom_node_unref(r);
	return v;
}

static JSValue n_document(JSContext *ctx, JSValueConst this_val, int argc,
			  JSValueConst *argv)
{
	jsthread *t = T(ctx);
	if (t->htmlc == NULL || t->htmlc->document == NULL)
		return JS_NULL;
	return qjs_wrap_node(t, (dom_node *)t->htmlc->document);
}

static JSValue n_create(JSContext *ctx, JSValueConst this_val, int argc,
			JSValueConst *argv, int which)
{
	jsthread *t = T(ctx);
	dom_document *doc;
	dom_string *s;
	dom_node *r = NULL;
	dom_exception exc = DOM_NO_ERR;
	JSValue v;
	if (t->htmlc == NULL || (doc = t->htmlc->document) == NULL)
		return JS_NULL;
	s = qjs_todstr(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
	if (s == NULL)
		return JS_EXCEPTION;
	switch (which) {
	case 0: exc = dom_document_create_element(doc, s, (dom_element **)&r); break;
	case 1: exc = dom_document_create_text_node(doc, s, (dom_text **)&r); break;
	case 2: exc = dom_document_create_comment(doc, s, (dom_comment **)&r); break;
	case 3: exc = dom_document_get_element_by_id(doc, s, (dom_element **)&r); break;
	case 4: exc = dom_document_create_document_fragment(doc,
				(dom_document_fragment **)&r); break;
	}
	dom_string_unref(s);
	if (exc != DOM_NO_ERR) {
		if (which == 3)
			return JS_NULL;
		return JS_ThrowInternalError(ctx, "create error %d", exc);
	}
	if (r == NULL)
		return JS_NULL;
	v = qjs_wrap_node(t, r);
	dom_node_unref(r);
	return v;
}

/* form control value / checked */
static JSValue n_value(JSContext *ctx, JSValueConst this_val, int argc,
		       JSValueConst *argv, int set)
{
	dom_html_element_type et = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
	dom_string *s = NULL;
	dom_exception exc = DOM_NOT_SUPPORTED_ERR;
	NODE_ARG(n, 0);
	dom_html_element_get_tag_type((dom_html_element *)n, &et);
	if (set) {
		s = qjs_todstr(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
		if (s == NULL)
			return JS_EXCEPTION;
	}
	switch (et) {
	case DOM_HTML_ELEMENT_TYPE_INPUT:
		exc = set ? dom_html_input_element_set_value(
				(dom_html_input_element *)n, s)
			  : dom_html_input_element_get_value(
				(dom_html_input_element *)n, &s);
		break;
	case DOM_HTML_ELEMENT_TYPE_TEXTAREA:
		exc = set ? dom_html_text_area_element_set_value(
				(dom_html_text_area_element *)n, s)
			  : dom_html_text_area_element_get_value(
				(dom_html_text_area_element *)n, &s);
		break;
	case DOM_HTML_ELEMENT_TYPE_SELECT:
		exc = set ? dom_html_select_element_set_value(
				(dom_html_select_element *)n, s)
			  : dom_html_select_element_get_value(
				(dom_html_select_element *)n, &s);
		break;
	default:
		break;
	}
	if (set) {
		dom_string_unref(s);
		return JS_NewBool(ctx, exc == DOM_NO_ERR);
	}
	if (exc != DOM_NO_ERR)
		return JS_UNDEFINED;
	return s ? qjs_dstr(ctx, s) : JS_NewString(ctx, "");
}

static JSValue n_checked(JSContext *ctx, JSValueConst this_val, int argc,
			 JSValueConst *argv)
{
	bool b = false;
	NODE_ARG(n, 0);
	if (argc > 1 && !JS_IsUndefined(argv[1])) {
		dom_html_input_element_set_checked((dom_html_input_element *)n,
						   JS_ToBool(ctx, argv[1]));
		qjs_mark_dirty(ctx, n);
		return JS_UNDEFINED;
	}
	dom_html_input_element_get_checked((dom_html_input_element *)n, &b);
	return JS_NewBool(ctx, b);
}

/* listen(node, type) */
static JSValue n_listen(JSContext *ctx, JSValueConst this_val, int argc,
			JSValueConst *argv)
{
	dom_string *s;
	bool ok;
	NODE_ARG(n, 0);
	s = qjs_todstr(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
	if (s == NULL)
		return JS_EXCEPTION;
	ok = qjs_listen(T(ctx), n, s);
	dom_string_unref(s);
	return JS_NewBool(ctx, ok);
}

/* ------------------------------------------------------------------ */
/* natives: events                                                      */

static JSValue n_ev(JSContext *ctx, JSValueConst this_val, int argc,
		    JSValueConst *argv, int which)
{
	dom_event *e = argc > 0 ? qjs_event(ctx, argv[0]) : NULL;
	dom_event_target *et = NULL;
	dom_event_flow_phase ph = 0;
	bool b = false;
	JSValue v;
	if (e == NULL)
		return JS_UNDEFINED;
	switch (which) {
	case 0:
	case 1:
		if (which == 0)
			dom_event_get_target(e, &et);
		else
			dom_event_get_current_target(e, &et);
		if (et == NULL)
			return JS_NULL;
		v = qjs_wrap_node(T(ctx), (dom_node *)et);
		dom_node_unref((dom_node *)et);
		return v;
	case 2:
		dom_event_get_event_phase(e, &ph);
		return JS_NewInt32(ctx, ph);
	case 3:
		dom_event_prevent_default(e);
		return JS_UNDEFINED;
	case 4:
		dom_event_stop_propagation(e);
		return JS_UNDEFINED;
	case 5:
		dom_event_stop_immediate_propagation(e);
		return JS_UNDEFINED;
	case 6:
		dom_event_is_default_prevented(e, &b);
		return JS_NewBool(ctx, b);
	}
	return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ */
/* natives: console, document.write                                     */

static JSValue n_log(JSContext *ctx, JSValueConst this_val, int argc,
		     JSValueConst *argv)
{
	jsthread *t = T(ctx);
	int32_t level = 1;
	size_t len;
	const char *s;
	browser_window_console_flags fl;
	if (argc > 0)
		JS_ToInt32(ctx, &level, argv[0]);
	s = JS_ToCStringLen(ctx, &len, argc > 1 ? argv[1] : JS_UNDEFINED);
	if (s == NULL)
		return JS_EXCEPTION;
	switch (level) {
	case 0: fl = BW_CS_FLAG_LEVEL_DEBUG; break;
	case 2: fl = BW_CS_FLAG_LEVEL_INFO; break;
	case 3: fl = BW_CS_FLAG_LEVEL_WARN; break;
	case 4: fl = BW_CS_FLAG_LEVEL_ERROR; break;
	default: fl = BW_CS_FLAG_LEVEL_LOG; break;
	}
	NSLOG(netsurf, DEBUG, "QJS console: %.*s", (int)len, s);
	fprintf(stderr, "[console] %.*s\n", (int)len, s);
	if (t->bw != NULL && !t->closed)
		browser_window_console_log(t->bw, BW_CS_SCRIPT_CONSOLE, s, len, fl);
	JS_FreeCString(ctx, s);
	return JS_UNDEFINED;
}

static JSValue n_docwrite(JSContext *ctx, JSValueConst this_val, int argc,
			  JSValueConst *argv)
{
	jsthread *t = T(ctx);
	size_t len;
	const char *s;
	if (t->htmlc == NULL || t->htmlc->parser == NULL)
		return JS_FALSE;
	s = JS_ToCStringLen(ctx, &len, argc > 0 ? argv[0] : JS_UNDEFINED);
	if (s == NULL)
		return JS_EXCEPTION;
	dom_hubbub_parser_insert_chunk(t->htmlc->parser, (const uint8_t *)s, len);
	JS_FreeCString(ctx, s);
	return JS_TRUE;
}

/* ------------------------------------------------------------------ */
/* natives: timers                                                      */

static void qjs_timer_cb(void *p)
{
	struct qjs_timer *tm = p;
	jsthread *t = tm->t;
	JSValue id;
	if (t == NULL || t->closed)
		return;
	if (tm->repeat)
		guit->misc->schedule(tm->ms, qjs_timer_cb, tm);
	id = JS_NewInt32(t->ctx, tm->id);
	if (!tm->repeat) {
		/* unlink first: the callback may set new timers */
		struct qjs_timer **pp = &t->timers;
		while (*pp && *pp != tm) pp = &(*pp)->next;
		if (*pp) *pp = tm->next;
		free(tm);
	}
	qjs_call(t, t->fn_timer, JS_UNDEFINED, 1, &id, "timer");
}

/* timer(id, ms, repeat) */
static JSValue n_timer(JSContext *ctx, JSValueConst this_val, int argc,
		       JSValueConst *argv)
{
	jsthread *t = T(ctx);
	struct qjs_timer *tm;
	int32_t id = 0, ms = 0;
	if (t->closed)
		return JS_FALSE;
	JS_ToInt32(ctx, &id, argv[0]);
	JS_ToInt32(ctx, &ms, argv[1]);
	tm = calloc(1, sizeof(*tm));
	if (tm == NULL)
		return JS_FALSE;
	tm->repeat = JS_ToBool(ctx, argv[2]);
	if (ms < 0) ms = 0;
	if (tm->repeat && ms < 10) ms = 10;
	tm->t = t;
	tm->id = id;
	tm->ms = ms;
	tm->next = t->timers;
	t->timers = tm;
	guit->misc->schedule(ms, qjs_timer_cb, tm);
	return JS_TRUE;
}

static JSValue n_cancel(JSContext *ctx, JSValueConst this_val, int argc,
			JSValueConst *argv)
{
	jsthread *t = T(ctx);
	int32_t id = 0;
	struct qjs_timer **pp = &t->timers;
	JS_ToInt32(ctx, &id, argv[0]);
	while (*pp) {
		if ((*pp)->id == id) {
			struct qjs_timer *tm = *pp;
			*pp = tm->next;
			guit->misc->schedule(-1, qjs_timer_cb, tm);
			free(tm);
			break;
		}
		pp = &(*pp)->next;
	}
	return JS_UNDEFINED;
}

static void qjs_cancel_all_timers(jsthread *t)
{
	while (t->timers) {
		struct qjs_timer *tm = t->timers;
		t->timers = tm->next;
		guit->misc->schedule(-1, qjs_timer_cb, tm);
		free(tm);
	}
}

/* ------------------------------------------------------------------ */
/* natives: location, cookies, misc                                     */

static nsurl *qjs_page_url(jsthread *t)
{
	if (t->htmlc == NULL)
		return NULL;
	if (t->htmlc->base.llcache != NULL)
		return llcache_handle_get_url(t->htmlc->base.llcache);
	return t->htmlc->base_url;
}

static JSValue n_href(JSContext *ctx, JSValueConst this_val, int argc,
		      JSValueConst *argv)
{
	nsurl *u = qjs_page_url(T(ctx));
	if (u == NULL)
		return JS_NewString(ctx, "about:blank");
	return JS_NewStringLen(ctx, nsurl_access(u), nsurl_length(u));
}

static JSValue n_baseurl(JSContext *ctx, JSValueConst this_val, int argc,
			 JSValueConst *argv)
{
	jsthread *t = T(ctx);
	nsurl *u = t->htmlc ? t->htmlc->base_url : NULL;
	if (u == NULL)
		u = qjs_page_url(t);
	if (u == NULL)
		return JS_NewString(ctx, "about:blank");
	return JS_NewStringLen(ctx, nsurl_access(u), nsurl_length(u));
}

/* resolve(rel) -> absolute string against document base */
static JSValue n_resolve(JSContext *ctx, JSValueConst this_val, int argc,
			 JSValueConst *argv)
{
	jsthread *t = T(ctx);
	nsurl *base = t->htmlc ? t->htmlc->base_url : NULL;
	nsurl *r = NULL;
	const char *s;
	JSValue v;
	if (base == NULL)
		base = qjs_page_url(t);
	s = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
	if (s == NULL)
		return JS_EXCEPTION;
	if (base != NULL)
		nsurl_join(base, s, &r);
	else
		nsurl_create(s, &r);
	JS_FreeCString(ctx, s);
	if (r == NULL)
		return JS_NULL;
	v = JS_NewStringLen(ctx, nsurl_access(r), nsurl_length(r));
	nsurl_unref(r);
	return v;
}

struct qjs_nav {
	struct browser_window *bw;
	nsurl *url;
	nsurl *ref;
	bool reload;
};

static void qjs_nav_cb(void *p)
{
	struct qjs_nav *nv = p;
	if (nv->reload)
		browser_window_reload(nv->bw, false);
	else
		browser_window_navigate(nv->bw, nv->url, nv->ref,
				BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
	if (nv->url) nsurl_unref(nv->url);
	if (nv->ref) nsurl_unref(nv->ref);
	free(nv);
}

/* navigate(url|null) ; null = reload.  Deferred so we never tear down the
 * content we are running inside of. */
static JSValue n_navigate(JSContext *ctx, JSValueConst this_val, int argc,
			  JSValueConst *argv)
{
	jsthread *t = T(ctx);
	struct qjs_nav *nv;
	nsurl *page = qjs_page_url(t);
	if (t->bw == NULL || t->closed)
		return JS_FALSE;
	nv = calloc(1, sizeof(*nv));
	if (nv == NULL)
		return JS_FALSE;
	nv->bw = t->bw;
	if (argc < 1 || JS_IsNull(argv[0])) {
		nv->reload = true;
	} else {
		const char *s = JS_ToCString(ctx, argv[0]);
		if (s == NULL) {
			free(nv);
			return JS_EXCEPTION;
		}
		if (page != NULL)
			nsurl_join(page, s, &nv->url);
		else
			nsurl_create(s, &nv->url);
		JS_FreeCString(ctx, s);
		if (nv->url == NULL) {
			free(nv);
			return JS_FALSE;
		}
		if (page) nv->ref = nsurl_ref(page);
	}
	guit->misc->schedule(0, qjs_nav_cb, nv);
	return JS_TRUE;
}

static JSValue n_cookie(JSContext *ctx, JSValueConst this_val, int argc,
			JSValueConst *argv)
{
	jsthread *t = T(ctx);
	nsurl *u = qjs_page_url(t);
	if (u == NULL)
		return JS_NewString(ctx, "");
	if (argc > 0 && !JS_IsUndefined(argv[0])) {
		const char *s = JS_ToCString(ctx, argv[0]);
		if (s == NULL)
			return JS_EXCEPTION;
		urldb_set_cookie(s, u, NULL);
		JS_FreeCString(ctx, s);
		return JS_UNDEFINED;
	} else {
		char *c = urldb_get_cookie(u, false);
		JSValue v;
		if (c == NULL)
			return JS_NewString(ctx, "");
		v = JS_NewString(ctx, c);
		free(c);
		return v;
	}
}

static JSValue n_ua(JSContext *ctx, JSValueConst this_val, int argc,
		    JSValueConst *argv)
{
	return JS_NewString(ctx, user_agent_string());
}

static JSValue n_dims(JSContext *ctx, JSValueConst this_val, int argc,
		      JSValueConst *argv)
{
	jsthread *t = T(ctx);
	int w = 1024, h = 768;
	JSValue a = JS_NewArray(ctx);
	if (t->bw != NULL && !t->closed)
		browser_window_get_dimensions(t->bw, &w, &h);
	JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, w));
	JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, h));
	return a;
}

static JSValue n_now(JSContext *ctx, JSValueConst this_val, int argc,
		     JSValueConst *argv)
{
	uint64_t ms = 0;
	nsu_getmonotonic_ms(&ms);
	return JS_NewFloat64(ctx, (double)ms);
}

/* ------------------------------------------------------------------ */
/* natives: localStorage (one JSON file per origin)                     */

static char *qjs_storage_path(jsthread *t)
{
	nsurl *u = qjs_page_url(t);
	const char *home = getenv("HOME");
	char dir[512], *path;
	lwc_string *host;
	const char *h;
	size_t i, n;
	if (u == NULL)
		return NULL;
	host = nsurl_get_component(u, NSURL_HOST);
	h = host ? lwc_string_data(host) : "local";
	if (home == NULL || home[0] == '\0')
		home = "/tmp";
	snprintf(dir, sizeof(dir), "%s/.netsurf", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.netsurf/storage", home);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		snprintf(dir, sizeof(dir), "/tmp/ns_storage");
		mkdir(dir, 0755);
	}
	n = strlen(dir) + strlen(h) + 8;
	path = malloc(n);
	if (path != NULL) {
		char *p;
		snprintf(path, n, "%s/%s.json", dir, h);
		/* sanitise host part */
		p = path + strlen(dir) + 1;
		for (i = 0; p[i] && p[i] != '.'; i++) ;
		for (i = 0; p[i]; i++) {
			if (!(isalnum((unsigned char)p[i]) || p[i] == '.' ||
			      p[i] == '-' || p[i] == '_'))
				p[i] = '_';
		}
	}
	if (host) lwc_string_unref(host);
	return path;
}

static JSValue n_storage(JSContext *ctx, JSValueConst this_val, int argc,
			 JSValueConst *argv)
{
	jsthread *t = T(ctx);
	char *path = qjs_storage_path(t);
	FILE *f;
	JSValue v = JS_NULL;
	if (path == NULL)
		return JS_NULL;
	if (argc > 0 && !JS_IsUndefined(argv[0])) {
		size_t len;
		const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
		if (s != NULL) {
			char tmp[600];
			snprintf(tmp, sizeof(tmp), "%s.tmp", path);
			f = fopen(tmp, "wb");
			if (f != NULL) {
				fwrite(s, 1, len, f);
				fclose(f);
				rename(tmp, path);
			}
			JS_FreeCString(ctx, s);
		}
		v = JS_UNDEFINED;
	} else {
		f = fopen(path, "rb");
		if (f != NULL) {
			char *buf;
			long sz;
			fseek(f, 0, SEEK_END);
			sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (sz > 0 && sz < (8 << 20) &&
			    (buf = malloc(sz)) != NULL) {
				if (fread(buf, 1, sz, f) == (size_t)sz)
					v = JS_NewStringLen(ctx, buf, sz);
				free(buf);
			}
			fclose(f);
		}
	}
	free(path);
	return v;
}

/* ------------------------------------------------------------------ */
/* natives: fetch via llcache                                           */

static void qjs_fetch_release_cb(void *p)
{
	struct qjs_fetch *f = p;
	if (f->h != NULL)
		llcache_handle_release(f->h);
	free(f->post);
	free(f);
}

static void qjs_fetch_unlink(struct qjs_fetch *f)
{
	if (f->t != NULL) {
		struct qjs_fetch **pp = &f->t->fetches;
		while (*pp && *pp != f) pp = &(*pp)->next;
		if (*pp) *pp = f->next;
	}
}

static void qjs_fetch_finish(struct qjs_fetch *f, bool ok, const char *err)
{
	jsthread *t = f->t;
	f->done = true;
	qjs_fetch_unlink(f);
	if (t != NULL && !t->closed) {
		JSContext *ctx = t->ctx;
		JSValue argv[7];
		size_t len = 0;
		const uint8_t *data = NULL;
		const char *ct = NULL;
		nsurl *u = llcache_handle_get_url(f->h);
		if (ok)
			data = llcache_handle_get_source_data(f->h, &len);
		ct = llcache_handle_get_header(f->h, "Content-Type");
		argv[0] = JS_NewInt32(ctx, f->id);
		argv[1] = JS_NewBool(ctx, ok);
		argv[2] = JS_NewInt32(ctx, ok ? 200 : 0);
		argv[3] = u ? JS_NewStringLen(ctx, nsurl_access(u),
					      nsurl_length(u)) : JS_NULL;
		argv[4] = JS_NewStringLen(ctx, data ? (const char *)data : "",
					  data ? len : 0);
		argv[5] = ct ? JS_NewString(ctx, ct) : JS_NULL;
		argv[6] = err ? JS_NewString(ctx, err) : JS_NULL;
		qjs_call(t, t->fn_fetchdone, JS_UNDEFINED, 7, argv, "fetch");
		for (int i = 3; i < 7; i++)
			JS_FreeValue(ctx, argv[i]);
	}
	guit->misc->schedule(0, qjs_fetch_release_cb, f);
}

static nserror qjs_fetch_cb(llcache_handle *handle,
			    const llcache_event *event, void *pw)
{
	struct qjs_fetch *f = pw;
	if (f->done)
		return NSERROR_OK;
	switch (event->type) {
	case LLCACHE_EVENT_DONE:
		qjs_fetch_finish(f, true, NULL);
		break;
	case LLCACHE_EVENT_ERROR:
		qjs_fetch_finish(f, false, event->data.error.msg ?
				 event->data.error.msg : "network error");
		break;
	default:
		break;
	}
	return NSERROR_OK;
}

/* fetch(id, url, method, body|null) */
static JSValue n_fetch(JSContext *ctx, JSValueConst this_val, int argc,
		       JSValueConst *argv)
{
	jsthread *t = T(ctx);
	struct qjs_fetch *f;
	int32_t id = 0;
	const char *s, *m;
	nsurl *url = NULL, *page = qjs_page_url(t);
	llcache_post_data post;
	bool is_post;
	nserror err;

	if (t->closed)
		return JS_FALSE;
	JS_ToInt32(ctx, &id, argv[0]);
	s = JS_ToCString(ctx, argv[1]);
	if (s == NULL)
		return JS_EXCEPTION;
	if (page != NULL)
		nsurl_join(page, s, &url);
	else
		nsurl_create(s, &url);
	JS_FreeCString(ctx, s);
	if (url == NULL)
		return JS_ThrowTypeError(ctx, "bad URL");

	m = JS_ToCString(ctx, argv[2]);
	is_post = m && strcasecmp(m, "GET") != 0 && strcasecmp(m, "HEAD") != 0;
	if (m) JS_FreeCString(ctx, m);

	f = calloc(1, sizeof(*f));
	if (f == NULL) {
		nsurl_unref(url);
		return JS_FALSE;
	}
	f->t = t;
	f->id = id;
	if (is_post) {
		size_t len = 0;
		const char *b = JS_IsNull(argv[3]) || JS_IsUndefined(argv[3])
			? NULL : JS_ToCStringLen(ctx, &len, argv[3]);
		f->post = strdup(b ? b : "");
		if (b) JS_FreeCString(ctx, b);
		post.type = LLCACHE_POST_URL_ENCODED;
		post.data.urlenc = f->post;
	}
	f->next = t->fetches;
	t->fetches = f;
	err = llcache_handle_retrieve(url,
			LLCACHE_RETRIEVE_NO_ERROR_PAGES |
			(is_post ? LLCACHE_RETRIEVE_FORCE_FETCH : 0),
			page, is_post ? &post : NULL,
			qjs_fetch_cb, f, &f->h);
	nsurl_unref(url);
	if (err != NSERROR_OK) {
		qjs_fetch_unlink(f);
		free(f->post);
		free(f);
		return JS_FALSE;
	}
	return JS_TRUE;
}

static void qjs_abort_fetches(jsthread *t)
{
	while (t->fetches) {
		struct qjs_fetch *f = t->fetches;
		t->fetches = f->next;
		f->t = NULL;
		f->done = true;
		if (f->h) llcache_handle_abort(f->h);
		guit->misc->schedule(0, qjs_fetch_release_cb, f);
	}
}

/* ------------------------------------------------------------------ */
/* native table                                                         */

#define FM(name, len, fn, magic) \
	JS_CFUNC_MAGIC_DEF(name, len, fn, magic)

static const JSCFunctionListEntry qjs_natives[] = {
	FM("parent", 1, n_nav, 0),
	FM("first", 1, n_nav, 1),
	FM("last", 1, n_nav, 2),
	FM("next", 1, n_nav, 3),
	FM("prev", 1, n_nav, 4),
	FM("ownerDoc", 1, n_nav, 5),
	FM("nodeName", 1, n_info, 0),
	FM("nodeValue", 1, n_info, 1),
	FM("text", 1, n_info, 2),
	FM("setText", 2, n_set_text, 0),
	FM("setValue", 2, n_set_text, 1),
	JS_CFUNC_DEF("nodeType", 1, n_type),
	FM("getAttr", 2, n_attr, 0),
	FM("setAttr", 3, n_attr, 1),
	FM("removeAttr", 2, n_attr, 2),
	FM("hasAttr", 2, n_attr, 3),
	JS_CFUNC_DEF("attrs", 1, n_attrs),
	FM("append", 2, n_mutate, 0),
	FM("insertBefore", 3, n_mutate, 1),
	FM("remove", 2, n_mutate, 2),
	FM("replace", 3, n_mutate, 3),
	JS_CFUNC_DEF("clone", 2, n_clone),
	JS_CFUNC_DEF("document", 0, n_document),
	FM("createElement", 1, n_create, 0),
	FM("createText", 1, n_create, 1),
	FM("createComment", 1, n_create, 2),
	FM("byId", 1, n_create, 3),
	FM("createFragment", 1, n_create, 4),
	FM("getValue", 1, n_value, 0),
	FM("setFormValue", 2, n_value, 1),
	JS_CFUNC_DEF("checked", 2, n_checked),
	JS_CFUNC_DEF("listen", 2, n_listen),
	FM("evTarget", 1, n_ev, 0),
	FM("evCurrent", 1, n_ev, 1),
	FM("evPhase", 1, n_ev, 2),
	FM("evPrevent", 1, n_ev, 3),
	FM("evStop", 1, n_ev, 4),
	FM("evStopImm", 1, n_ev, 5),
	FM("evPrevented", 1, n_ev, 6),
	JS_CFUNC_DEF("log", 2, n_log),
	JS_CFUNC_DEF("docWrite", 1, n_docwrite),
	JS_CFUNC_DEF("timer", 3, n_timer),
	JS_CFUNC_DEF("cancel", 1, n_cancel),
	JS_CFUNC_DEF("href", 0, n_href),
	JS_CFUNC_DEF("baseURL", 0, n_baseurl),
	JS_CFUNC_DEF("resolve", 1, n_resolve),
	JS_CFUNC_DEF("navigate", 1, n_navigate),
	JS_CFUNC_DEF("cookie", 1, n_cookie),
	JS_CFUNC_DEF("ua", 0, n_ua),
	JS_CFUNC_DEF("dims", 0, n_dims),
	JS_CFUNC_DEF("now", 0, n_now),
	JS_CFUNC_DEF("storage", 1, n_storage),
	JS_CFUNC_DEF("fetch", 4, n_fetch),
};

/* ------------------------------------------------------------------ */
/* jobs                                                                 */

static void qjs_run_jobs(jsthread *t)
{
	JSContext *c2;
	int n = 0;
	for (;;) {
		int r = JS_ExecutePendingJob(t->heap->rt, &c2);
		if (r <= 0) {
			if (r < 0) {
				jsthread *t2 = JS_GetContextOpaque(c2);
				qjs_log_exception(t2 ? t2 : t, "promise job");
				continue;
			}
			break;
		}
		if (++n > 100000)
			break;
	}
}

/* ------------------------------------------------------------------ */
/* js.h interface                                                       */

void js_initialise(void)
{
	/* registers the application/javascript content handler */
	javascript_init();
	NSLOG(netsurf, INFO, "QuickJS %s initialised", QJS_ENGINE_VERSION);
}

void js_finalise(void)
{
}

nserror js_newheap(int timeout, jsheap **heap)
{
	jsheap *h = calloc(1, sizeof(*h));
	if (h == NULL)
		return NSERROR_NOMEM;
	h->rt = JS_NewRuntime();
	if (h->rt == NULL) {
		free(h);
		return NSERROR_NOMEM;
	}
	h->timeout = timeout;
	JS_SetMaxStackSize(h->rt, QJS_STACK_SIZE);
	JS_SetMemoryLimit(h->rt, 256u << 20);
	JS_SetGCThreshold(h->rt, 8u << 20);
	JS_SetModuleLoaderFunc(h->rt, qjs_mod_normalize, qjs_mod_load, NULL);
	JS_SetInterruptHandler(h->rt, qjs_interrupt, h);
	if (qjs_node_class_id == 0)
		QJS_NEW_CLASS_ID(h->rt, &qjs_node_class_id);
	if (qjs_event_class_id == 0)
		QJS_NEW_CLASS_ID(h->rt, &qjs_event_class_id);
	JS_NewClass(h->rt, qjs_node_class_id, &qjs_node_class);
	JS_NewClass(h->rt, qjs_event_class_id, &qjs_event_class);
	*heap = h;
	NSLOG(netsurf, DEEPDEBUG, "QJS new heap %p", h);
	return NSERROR_OK;
}

static void qjs_heap_free(jsheap *h)
{
	JS_FreeRuntime(h->rt);
	free(h);
}

void js_destroyheap(jsheap *heap)
{
	if (heap == NULL)
		return;
	heap->pending_destroy = true;
	if (heap->live_threads == 0)
		qjs_heap_free(heap);
}

static JSValue qjs_getfn(JSContext *ctx, JSValueConst ns, const char *name)
{
	return JS_GetPropertyStr(ctx, ns, name);
}

nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv,
		     jsthread **thread)
{
	jsthread *t;
	JSContext *ctx;
	JSValue global, ns, r;

	if (heap == NULL || heap->pending_destroy)
		return NSERROR_BAD_PARAMETER;
	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NSERROR_NOMEM;
	ctx = JS_NewContext(heap->rt);
	if (ctx == NULL) {
		free(t);
		return NSERROR_NOMEM;
	}
	t->heap = heap;
	t->ctx = ctx;
	t->bw = win_priv;
	t->htmlc = doc_priv;
	if (t->htmlc != NULL)
		qjs_rebox_close(t->htmlc); /* stale state for a reused address */
	t->fn_proto = t->fn_dispatch = t->fn_window = JS_UNDEFINED;
	t->fn_timer = t->fn_fetchdone = t->fn_evproto = JS_UNDEFINED;
	JS_SetContextOpaque(ctx, t);

	t->pw_cap.t = t; t->pw_cap.capture = true;
	t->pw_bub.t = t; t->pw_bub.capture = false;
	dom_event_listener_create(qjs_generic_handler, &t->pw_cap, &t->l_cap);
	dom_event_listener_create(qjs_generic_handler, &t->pw_bub, &t->l_bub);

	heap->live_threads++;

	global = JS_GetGlobalObject(ctx);
	ns = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, ns, qjs_natives,
				   QJS_COUNTOF(qjs_natives));
	JS_SetPropertyStr(ctx, global, "__ns", ns);	/* takes ns */

	qjs_enter(t);
	{
		/* JS_Eval needs a NUL-terminated buffer; xxd output is not */
		char *src = malloc(dom_js_len + 1);
		if (src != NULL) {
			memcpy(src, dom_js, dom_js_len);
			src[dom_js_len] = '\0';
			r = JS_Eval(ctx, src, dom_js_len, "dom.js",
				    JS_EVAL_TYPE_GLOBAL);
			free(src);
		} else {
			r = JS_ThrowOutOfMemory(ctx);
		}
	}
	if (JS_IsException(r)) {
		qjs_log_exception(t, "dom.js");
	}
	JS_FreeValue(ctx, r);
	qjs_leave(t);

	/* dom.js stores its hooks on __nsHooks and deletes __ns */
	ns = JS_GetPropertyStr(ctx, global, "__nsHooks");
	t->fn_proto = qjs_getfn(ctx, ns, "proto");
	t->fn_dispatch = qjs_getfn(ctx, ns, "dispatch");
	t->fn_window = qjs_getfn(ctx, ns, "window");
	t->fn_timer = qjs_getfn(ctx, ns, "timer");
	t->fn_fetchdone = qjs_getfn(ctx, ns, "fetchdone");
	t->fn_evproto = qjs_getfn(ctx, ns, "evproto");
	JS_FreeValue(ctx, ns);
	{
		JSAtom a = JS_NewAtom(ctx, "__nsHooks");
		JS_DeleteProperty(ctx, global, a, 0);
		JS_FreeAtom(ctx, a);
	}
	JS_FreeValue(ctx, global);

	if (!JS_IsFunction(ctx, t->fn_proto)) {
		NSLOG(netsurf, ERROR, "QJS: dom.js failed to initialise");
	}
	*thread = t;
	NSLOG(netsurf, DEEPDEBUG, "QJS new thread %p", t);
	return NSERROR_OK;
}

nserror js_closethread(jsthread *thread)
{
	if (thread == NULL)
		return NSERROR_BAD_PARAMETER;
	if (thread->closed)
		return NSERROR_OK;
	thread->closed = true;
	if (thread->htmlc != NULL)
		qjs_rebox_close(thread->htmlc);
	qjs_cancel_all_timers(thread);
	qjs_abort_fetches(thread);
	qjs_unlisten_all(thread);
	return NSERROR_OK;
}

static void qjs_thread_free(jsthread *t)
{
	JSContext *ctx = t->ctx;
	jsheap *heap = t->heap;
	size_t i;

	if (!t->closed)
		js_closethread(t);

	JS_FreeValue(ctx, t->fn_proto);
	JS_FreeValue(ctx, t->fn_dispatch);
	JS_FreeValue(ctx, t->fn_window);
	JS_FreeValue(ctx, t->fn_timer);
	JS_FreeValue(ctx, t->fn_fetchdone);
	JS_FreeValue(ctx, t->fn_evproto);
	for (i = 0; i < t->evs_cnt; i++)
		JS_FreeValue(ctx, t->evs[i].obj);
	free(t->evs);
	for (i = 0; i < t->nodes_cap; i++) {
		if (t->nodes[i].node != NULL)
			JS_FreeValue(ctx, t->nodes[i].obj);
	}
	free(t->nodes);
	JS_FreeContext(ctx);
	JS_RunGC(heap->rt);
	if (t->l_cap) dom_event_listener_unref(t->l_cap);
	if (t->l_bub) dom_event_listener_unref(t->l_bub);
	free(t);

	heap->live_threads--;
	if (heap->pending_destroy && heap->live_threads == 0)
		qjs_heap_free(heap);
}

static void qjs_leave(jsthread *t)
{
	t->in_use--;
	if (t->in_use == 0) {
		t->heap->exec_start = 0;
		if (t->pending_destroy)
			qjs_thread_free(t);
	}
}

void js_destroythread(jsthread *thread)
{
	if (thread == NULL)
		return;
	thread->pending_destroy = true;
	if (thread->htmlc != NULL)
		qjs_rebox_abandon(thread->htmlc);
	if (thread->in_use == 0)
		qjs_thread_free(thread);
}

/* ---- ES module support (type=module) ----------------------------------
 * Static/dynamic imports are fetched synchronously with libcurl (blocking,
 * but module graphs are small on the sites we care about). */

static const char *qjs_doc_url(jsthread *t)
{
	if (t != NULL && t->htmlc != NULL && t->htmlc->base_url != NULL)
		return nsurl_access(t->htmlc->base_url);
	return "about:blank";
}

/* import maps: <script type="importmap"> stores its "imports" object on
 * the global as __ns_importmap; bare/exact specifiers are mapped (exact
 * key, then longest "prefix/" key).  Returns malloc'd string or NULL. */
static char *qjs_importmap_lookup(JSContext *ctx, const char *name)
{
	JSValue g = JS_GetGlobalObject(ctx);
	JSValue im = JS_GetPropertyStr(ctx, g, "__ns_importmap");
	JSValue v;
	char *res = NULL;
	JS_FreeValue(ctx, g);
	if (!JS_IsObject(im)) {
		JS_FreeValue(ctx, im);
		return NULL;
	}
	v = JS_GetPropertyStr(ctx, im, name);
	if (JS_IsString(v)) {
		const char *c = JS_ToCString(ctx, v);
		if (c) { res = strdup(c); JS_FreeCString(ctx, c); }
	} else {
		JSPropertyEnum *tab = NULL;
		uint32_t n = 0, i;
		size_t best = 0;
		if (JS_GetOwnPropertyNames(ctx, &tab, &n, im,
				JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
			for (i = 0; i < n; i++) {
				const char *k = JS_AtomToCString(ctx, tab[i].atom);
				size_t kl = k ? strlen(k) : 0;
				if (kl > best && k[kl - 1] == '/' &&
				    strncmp(name, k, kl) == 0) {
					JSValue pv = JS_GetProperty(ctx, im, tab[i].atom);
					const char *pc = JS_ToCString(ctx, pv);
					if (pc) {
						size_t pl = strlen(pc);
						char *r = malloc(pl + strlen(name + kl) + 1);
						if (r) {
							memcpy(r, pc, pl);
							strcpy(r + pl, name + kl);
							free(res);
							res = r;
							best = kl;
						}
						JS_FreeCString(ctx, pc);
					}
					JS_FreeValue(ctx, pv);
				}
				if (k) JS_FreeCString(ctx, k);
			}
			for (i = 0; i < n; i++)
				JS_FreeAtom(ctx, tab[i].atom);
			js_free(ctx, tab);
		}
	}
	JS_FreeValue(ctx, v);
	JS_FreeValue(ctx, im);
	return res;
}

bool js_importmap(jsthread *t, const uint8_t *txt, size_t len)
{
	JSValue j, imp, g, old;
	if (t == NULL || t->closed || t->pending_destroy || txt == NULL)
		return false;
	{
		char *b = malloc(len + 1);
		if (b == NULL) return false;
		memcpy(b, txt, len);
		b[len] = 0;
		qjs_enter(t);
		j = JS_ParseJSON(t->ctx, b, len, "importmap");
		free(b);
	}
	if (JS_IsException(j)) {
		qjs_log_exception(t, "importmap");
		qjs_leave(t);
		return false;
	}
	imp = JS_GetPropertyStr(t->ctx, j, "imports");
	g = JS_GetGlobalObject(t->ctx);
	old = JS_GetPropertyStr(t->ctx, g, "__ns_importmap");
	if (JS_IsObject(imp)) {
		if (JS_IsObject(old)) {
			/* merge: Object.assign(old, imp) */
			JSValue obj = JS_GetPropertyStr(t->ctx, g, "Object");
			JSValue as = JS_GetPropertyStr(t->ctx, obj, "assign");
			JSValue args[2] = { old, imp };
			JS_FreeValue(t->ctx, JS_Call(t->ctx, as, obj, 2, args));
			JS_FreeValue(t->ctx, as);
			JS_FreeValue(t->ctx, obj);
		} else {
			JS_DefinePropertyValueStr(t->ctx, g, "__ns_importmap",
					JS_DupValue(t->ctx, imp), JS_PROP_CONFIGURABLE);
		}
	}
	JS_FreeValue(t->ctx, old);
	JS_FreeValue(t->ctx, g);
	JS_FreeValue(t->ctx, imp);
	JS_FreeValue(t->ctx, j);
	qjs_leave(t);
	return true;
}

static char *qjs_mod_normalize(JSContext *ctx, const char *base,
			       const char *name, void *opaque)
{
	jsthread *t = JS_GetContextOpaque(ctx);
	nsurl *b = NULL, *j = NULL;
	char *out;
	char *mapped;
	(void)opaque;
	mapped = qjs_importmap_lookup(ctx, name);
	if (mapped != NULL) {
		name = mapped;
		base = qjs_doc_url(t);
	}
	if (base == NULL || nsurl_create(base, &b) != NSERROR_OK ||
	    strncmp(base, "?", 1) == 0) {
		if (b) { nsurl_unref(b); b = NULL; }
		if (nsurl_create(qjs_doc_url(t), &b) != NSERROR_OK) {
			out = js_strdup(ctx, name);
			free(mapped);
			return out;
		}
	}
	if (nsurl_join(b, name, &j) != NSERROR_OK) {
		nsurl_unref(b);
		out = js_strdup(ctx, name);
		free(mapped);
		return out;
	}
	out = js_strdup(ctx, nsurl_access(j));
	nsurl_unref(j);
	nsurl_unref(b);
	free(mapped);
	return out;
}

struct qjs_mbuf { char *p; size_t n, cap; };

static size_t qjs_mod_wr(char *d, size_t sz, size_t nm, void *u)
{
	struct qjs_mbuf *m = u;
	size_t len = sz * nm;
	if (m->n + len + 1 > (16u << 20))
		return 0;
	if (m->n + len + 1 > m->cap) {
		size_t nc = (m->cap ? m->cap * 2 : 65536);
		char *np;
		while (nc < m->n + len + 1) nc *= 2;
		np = realloc(m->p, nc);
		if (np == NULL) return 0;
		m->p = np; m->cap = nc;
	}
	memcpy(m->p + m->n, d, len);
	m->n += len;
	m->p[m->n] = 0;
	return len;
}

static char *qjs_mod_fetch(const char *url, size_t *len)
{
	struct qjs_mbuf m = { NULL, 0, 0 };
	CURL *c = curl_easy_init();
	long code = 0;
	CURLcode rc;
	if (c == NULL) return NULL;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent_string());
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, qjs_mod_wr);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
	if (nsoption_charp(ca_bundle) && *nsoption_charp(ca_bundle))
		curl_easy_setopt(c, CURLOPT_CAINFO, nsoption_charp(ca_bundle));
	if (nsoption_charp(ca_path) && *nsoption_charp(ca_path))
		curl_easy_setopt(c, CURLOPT_CAPATH, nsoption_charp(ca_path));
	rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK || (code != 0 && (code < 200 || code >= 300))) {
		fprintf(stderr, "[qjs] module fetch failed %s rc=%d http=%ld\n",
			url, (int)rc, code);
		free(m.p);
		return NULL;
	}
	if (m.p == NULL) { m.p = strdup(""); }
	*len = m.n;
	return m.p;
}

static void qjs_mod_meta(JSContext *ctx, JSValue fn, const char *url)
{
	JSModuleDef *m = JS_VALUE_GET_PTR(fn);
	JSValue meta = JS_GetImportMeta(ctx, m);
	if (!JS_IsException(meta)) {
		JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, url));
		JS_FreeValue(ctx, meta);
	}
}

static JSModuleDef *qjs_mod_load(JSContext *ctx, const char *name,
				 void *opaque)
{
	size_t len = 0;
	char *src;
	JSValue fn;
	JSModuleDef *m;
	(void)opaque;
	NSLOG(netsurf, INFO, "QJS import %s", name);
	src = qjs_mod_fetch(name, &len);
	if (src == NULL) {
		JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
		return NULL;
	}
	fn = JS_Eval(ctx, src, len, name,
		     JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	free(src);
	if (JS_IsException(fn))
		return NULL;
	qjs_mod_meta(ctx, fn, name);
	m = JS_VALUE_GET_PTR(fn);
	JS_FreeValue(ctx, fn);
	return m;
}

bool js_exec_module(jsthread *thread, const uint8_t *txt, size_t txtlen,
		    const char *name)
{
	JSValue fn, r;
	char *buf;
	bool ok = true;
	jsthread *t = thread;
	const char *nm = (name && name[0] != '?') ? name : qjs_doc_url(t);

	NSLOG(netsurf, INFO, "QJS module %p %s len %zu", t, nm, txtlen);
	if (t == NULL || t->closed || t->pending_destroy)
		return false;
	if (txt == NULL || txtlen == 0)
		return false;
	buf = malloc(txtlen + 1);
	if (buf == NULL)
		return false;
	memcpy(buf, txt, txtlen);
	buf[txtlen] = '\0';

	qjs_enter(t);
	JS_SetModuleLoaderFunc(t->heap->rt, qjs_mod_normalize, qjs_mod_load,
			       NULL);
	fn = JS_Eval(t->ctx, buf, txtlen, nm,
		     JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	free(buf);
	if (JS_IsException(fn)) {
		qjs_log_exception(t, nm);
		qjs_leave(t);
		return false;
	}
	qjs_mod_meta(t->ctx, fn, nm);
	r = JS_EvalFunction(t->ctx, fn);
	if (JS_IsException(r)) {
		qjs_log_exception(t, nm);
		ok = false;
	} else {
		qjs_run_jobs(t);
		if (JS_IsObject(r) &&
		    JS_PromiseState(t->ctx, r) == JS_PROMISE_REJECTED) {
			JS_Throw(t->ctx, JS_PromiseResult(t->ctx, r));
			qjs_log_exception(t, nm);
			ok = false;
		}
	}
	JS_FreeValue(t->ctx, r);
	qjs_run_jobs(t);
	qjs_leave(t);
	return ok;
}

bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen,
	     const char *name)
{
	JSValue r;
	char *buf;
	bool ok = true;
	jsthread *t = thread;

	NSLOG(netsurf, INFO, "QJS exec %p %s len %zu", t, name ? name : "-", txtlen);
	if (t == NULL || t->closed || t->pending_destroy)
		return false;
	if (txt == NULL || txtlen == 0)
		return false;
	buf = malloc(txtlen + 1);
	if (buf == NULL)
		return false;
	memcpy(buf, txt, txtlen);
	buf[txtlen] = '\0';

	qjs_enter(t);
	r = JS_Eval(t->ctx, buf, txtlen, name ? name : "script",
		    JS_EVAL_TYPE_GLOBAL);
	free(buf);
	if (JS_IsException(r)) {
		qjs_log_exception(t, name);
		ok = false;
	}
	JS_FreeValue(t->ctx, r);
	qjs_run_jobs(t);
	qjs_leave(t);
	return ok;
}

bool js_fire_event(jsthread *thread, const char *type,
		   struct dom_document *doc, struct dom_node *target)
{
	jsthread *t = thread;
	dom_event *evt = NULL;
	dom_string *ts = NULL;
	bool result = true;
	JSValue argv[2];

	if (t == NULL || t->closed || t->pending_destroy)
		return true;

	if (target == NULL && strcmp(type, "load") == 0) {
		argv[0] = JS_NewString(t->ctx, type);
		argv[1] = JS_UNDEFINED;
		qjs_call(t, t->fn_window, JS_UNDEFINED, 2, argv, "load");
		JS_FreeValue(t->ctx, argv[0]);
		if (getenv("NETSURF_QJS_DUMP") != NULL) {
			/* debugging aid: dump the DOM at load and 3s later */
			static const char dump[] =
				"(function(){var d=function(w){console.log('QJSDUMP '+w+' '+"
				"document.documentElement.outerHTML.length+' '+"
				"document.documentElement.outerHTML);};d('load');"
				"setTimeout(function(){d('3s');},3000);})();";
			js_exec(t, (const uint8_t *)dump, sizeof(dump) - 1,
				"dump");
		}
		return true;
	}

	if (dom_event_create(&evt) != DOM_NO_ERR)
		return true;
	if (dom_string_create((const uint8_t *)type, strlen(type), &ts)
	    != DOM_NO_ERR) {
		dom_event_unref(evt);
		return true;
	}
	dom_event_init(evt, ts, true, true);
	dom_string_unref(ts);

	if (target == NULL)
		target = (dom_node *)doc;
	t->in_use++;	/* keep alive across dispatch */
	dom_event_target_dispatch_event(target, evt, &result);
	if (strcmp(type, "DOMContentLoaded") == 0) {
		argv[0] = JS_NewString(t->ctx, type);
		argv[1] = qjs_wrap_event(t, evt);
		qjs_call(t, t->fn_window, JS_UNDEFINED, 2, argv, type);
		JS_FreeValue(t->ctx, argv[0]);
		JS_FreeValue(t->ctx, argv[1]);
	}
	js_event_cleanup(t, evt);
	dom_event_unref(evt);
	qjs_leave(t);
	return result;
}

bool js_dom_event_add_listener(jsthread *thread,
			       struct dom_document *document,
			       struct dom_node *node,
			       struct dom_string *event_type_dom,
			       void *js_funcval)
{
	return false;
}

void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	jsthread *t = thread;
	dom_namednodemap *map = NULL;
	dom_ulong len = 0, i;

	if (t == NULL || t->closed || node == NULL)
		return;
	if (dom_node_get_attributes(node, &map) != DOM_NO_ERR || map == NULL)
		return;
	dom_namednodemap_get_length(map, &len);
	for (i = 0; i < len; i++) {
		dom_node *a = NULL;
		dom_string *nm = NULL;
		if (dom_namednodemap_item(map, i, &a) != DOM_NO_ERR || a == NULL)
			continue;
		if (dom_attr_get_name((dom_attr *)a, &nm) == DOM_NO_ERR &&
		    nm != NULL) {
			const char *s = dom_string_data(nm);
			size_t l = dom_string_byte_length(nm);
			if (l > 2 && (s[0] == 'o' || s[0] == 'O') &&
			    (s[1] == 'n' || s[1] == 'N')) {
				dom_string *ev = NULL, *lc = NULL;
				if (dom_string_create((const uint8_t *)s + 2,
						      l - 2, &ev) == DOM_NO_ERR) {
					if (dom_string_tolower(ev, true, &lc)
					    == DOM_NO_ERR) {
						qjs_listen(t, (dom_node *)node, lc);
						dom_string_unref(lc);
					}
					dom_string_unref(ev);
				}
			}
			dom_string_unref(nm);
		}
		dom_node_unref(a);
	}
	dom_namednodemap_unref(map);
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	jsthread *t = thread;
	size_t i;
	if (t == NULL)
		return;
	for (i = 0; i < t->evs_cnt; i++) {
		if (t->evs[i].evt == evt) {
			JS_FreeValue(t->ctx, t->evs[i].obj);
			t->evs[i] = t->evs[--t->evs_cnt];
			return;
		}
	}
}
