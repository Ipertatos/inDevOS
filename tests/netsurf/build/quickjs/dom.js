/*
 * dom.js - DOM / browser API for the NetSurf QuickJS backend.
 *
 * Runs once per page context.  Talks to NetSurf only through the native
 * primitives on globalThis.__ns (deleted here) and exposes its hooks to
 * the C side through globalThis.__nsHooks (deleted by C afterwards).
 */
(function (N) {
'use strict';
const G = globalThis;
delete G.__ns;

const def = (o, name, get, set) => Object.defineProperty(o, name,
	{ get, set, configurable: true, enumerable: false });
const val = (o, name, v) => Object.defineProperty(o, name,
	{ value: v, writable: true, configurable: true, enumerable: false });
const isNode = (x) => x !== null && typeof x === 'object' && x instanceof Node;
/* shadow root -> host, host -> shadow root, shadow root -> options, element -> internals */
const SRH = new WeakMap(), SRS = new WeakMap(), SRM = new WeakMap(), EIS = new WeakMap();

/* ---------------- console ---------------- */
function fmt1(a) {
	if (typeof a === 'string') return a;
	if (a instanceof Error) {
		let r = a.name + ': ' + a.message;
		/* console.warn/error: keep the first frames, they are often the only clue */
		if (fmtStack && typeof a.stack === 'string' && a.stack)
			r += '\n' + a.stack.split('\n').filter(Boolean).slice(0, 8).join('\n');
		return r;
	}
	if (isNode(a)) return '[' + (a.nodeName || 'node') + (a.id ? '#' + a.id : '') + ']';
	if (typeof a === 'object' && a !== null) {
		try { return JSON.stringify(a); } catch (e) { return String(a); }
	}
	return String(a);
}
function fmt(args) {
	let s = '';
	let i = 0;
	if (typeof args[0] === 'string' && args[0].indexOf('%') >= 0 && args.length > 1) {
		i = 1;
		s = args[0].replace(/%[sdifoOc%]/g, (m) => {
			if (m === '%%') return '%';
			if (i >= args.length) return m;
			const a = args[i++];
			if (m === '%c') return '';
			if (m === '%d' || m === '%i') return String(parseInt(a));
			if (m === '%f') return String(parseFloat(a));
			return fmt1(a);
		});
	}
	for (; i < args.length; i++) s += (s.length ? ' ' : '') + fmt1(args[i]);
	return s;
}
const counts = {}, timesT = {};
let groupDepth = 0;
let fmtStack = false;
const mk = (lvl) => function (...a) {
	fmtStack = lvl >= 3;
	try { N.log(lvl, '  '.repeat(groupDepth) + fmt(a)); } finally { fmtStack = false; }
};
G.console = {
	log: mk(1), info: mk(2), warn: mk(3), error: mk(4), debug: mk(0),
	trace: mk(0), dir: mk(1), dirxml: mk(1), table: mk(1),
	group: function (...a) { if (a.length) mk(1)(...a); groupDepth++; },
	groupCollapsed: function (...a) { if (a.length) mk(1)(...a); groupDepth++; },
	groupEnd: function () { if (groupDepth) groupDepth--; },
	assert: function (c, ...a) { if (!c) mk(4)('Assertion failed:', ...a); },
	count: function (l = 'default') { counts[l] = (counts[l] || 0) + 1; mk(1)(l + ': ' + counts[l]); },
	countReset: function (l = 'default') { counts[l] = 0; },
	time: function (l = 'default') { timesT[l] = N.now(); },
	timeLog: function (l = 'default') { mk(1)(l + ': ' + (N.now() - (timesT[l] || 0)) + 'ms'); },
	timeEnd: function (l = 'default') { mk(1)(l + ': ' + (N.now() - (timesT[l] || 0)) + 'ms'); delete timesT[l]; },
	clear: function () {},
};

/* ---------------- events ---------------- */
const LST = new WeakMap();	/* target -> Map(type -> [listener]) */
const HND = new WeakMap();	/* target -> Map(type -> fn) (on* props) */
const INL = new WeakMap();	/* element -> Map(type -> {src, fn}) */
let winTarget = null;		/* set to window once defined */

function ensureListen(target, type) {
	if (target === winTarget) {
		if (type === 'load' || type === 'DOMContentLoaded') return;
		const d = G.document;
		if (d) N.listen(d, type);
	} else if (isNode(target)) {
		N.listen(target, type);
	}
}

const KEYCODES = { Enter: 13, Backspace: 8, Tab: 9, Escape: 27, ' ': 32,
	ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Delete: 46,
	Shift: 16, Control: 17, Alt: 18, Home: 36, End: 35, PageUp: 33, PageDown: 34 };

class Event {
	constructor(type, init) {
		if (arguments.length < 1) throw new TypeError('Event: type required');
		init = init || {};
		val(this, '__type', String(type));
		val(this, '__bubbles', !!init.bubbles);
		val(this, '__cancelable', !!init.cancelable);
		val(this, '__init', init);
		val(this, '__ts', N.now());
	}
	get type() { return this.__ni ? this.__ni.type : this.__type; }
	get bubbles() { return this.__ni ? this.__ni.bubbles : this.__bubbles; }
	get cancelable() { return this.__ni ? this.__ni.cancelable : this.__cancelable; }
	get isTrusted() { return this.__ni ? !!this.__ni.isTrusted : false; }
	get composed() { return false; }
	get timeStamp() { return this.__ts || N.now(); }
	get target() {
		if (this.__target !== undefined) return this.__target;
		const t = N.evTarget(this);
		return t === undefined ? null : t;
	}
	get srcElement() { return this.target; }
	get currentTarget() {
		if (this.__cur !== undefined) return this.__cur;
		const t = N.evCurrent(this);
		return t === undefined ? null : t;
	}
	get eventPhase() {
		if (this.__phase !== undefined) return this.__phase;
		const p = N.evPhase(this);
		return p === undefined ? 0 : p;
	}
	get defaultPrevented() { return !!this.__dp || N.evPrevented(this) === true; }
	get returnValue() { return !this.defaultPrevented; }
	set returnValue(v) { if (v === false) this.preventDefault(); }
	get cancelBubble() { return !!this.__stop; }
	set cancelBubble(v) { if (v) this.stopPropagation(); }
	preventDefault() { if (this.cancelable || this.__ni) { val(this, '__dp', true); N.evPrevent(this); } }
	stopPropagation() { val(this, '__stop', true); N.evStop(this); }
	stopImmediatePropagation() { val(this, '__stop', true); val(this, '__stopi', true); N.evStopImm(this); }
	composedPath() { return []; }
	initEvent(type, b, c) { val(this, '__type', String(type)); val(this, '__bubbles', !!b); val(this, '__cancelable', !!c); }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
class UIEvent extends Event {
	get view() { return G; }
	get detail() { return (this.__init && this.__init.detail) || 0; }
}
class CustomEvent extends Event {
	get detail() { return this.__init && this.__init.detail !== undefined ? this.__init.detail : null; }
	initCustomEvent(t, b, c, d) { this.initEvent(t, b, c); this.__init.detail = d; }
}
const initGet = (name, dflt) => function () {
	if (this.__ni && this.__ni[name] !== undefined) return this.__ni[name];
	if (this.__init && this.__init[name] !== undefined) return this.__init[name];
	return dflt;
};
class KeyboardEvent extends UIEvent {}
for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey', 'repeat', 'isComposing'])
	def(KeyboardEvent.prototype, k, initGet(k, false));
def(KeyboardEvent.prototype, 'key', initGet('key', ''));
def(KeyboardEvent.prototype, 'code', function () {
	const k = this.key;
	if (this.__init && this.__init.code) return this.__init.code;
	if (/^[a-zA-Z]$/.test(k)) return 'Key' + k.toUpperCase();
	if (/^[0-9]$/.test(k)) return 'Digit' + k;
	return k;
});
def(KeyboardEvent.prototype, 'location', initGet('location', 0));
def(KeyboardEvent.prototype, 'keyCode', function () {
	if (this.__init && this.__init.keyCode) return this.__init.keyCode;
	const k = this.key;
	if (KEYCODES[k]) return KEYCODES[k];
	if (k && k.length === 1) return k.toUpperCase().charCodeAt(0);
	return 0;
});
def(KeyboardEvent.prototype, 'which', function () { return this.keyCode; });
def(KeyboardEvent.prototype, 'charCode', function () {
	const k = this.key; return (this.type === 'keypress' && k && k.length === 1) ? k.charCodeAt(0) : 0;
});
KeyboardEvent.prototype.getModifierState = function () { return false; };
class MouseEvent extends UIEvent {}
for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) def(MouseEvent.prototype, k, initGet(k, false));
for (const k of ['clientX', 'clientY', 'screenX', 'screenY', 'pageX', 'pageY', 'offsetX', 'offsetY',
	'movementX', 'movementY', 'button', 'buttons']) def(MouseEvent.prototype, k, initGet(k, 0));
def(MouseEvent.prototype, 'which', function () { return this.button + 1; });
def(MouseEvent.prototype, 'relatedTarget', initGet('relatedTarget', null));
class PointerEvent extends MouseEvent {}
class FocusEvent extends UIEvent {}
class InputEvent extends UIEvent {}
def(InputEvent.prototype, 'data', initGet('data', null));
def(InputEvent.prototype, 'inputType', initGet('inputType', ''));
class ErrorEvent extends Event {}
class ProgressEvent extends Event {}
class MessageEvent extends Event {}
def(MessageEvent.prototype, 'data', initGet('data', null));
class StorageEvent extends Event {}
class PopStateEvent extends Event {}
def(PopStateEvent.prototype, 'state', initGet('state', null));
class HashChangeEvent extends Event {}
class TouchEvent extends UIEvent {}

function evproto(type) {
	if (/^key/.test(type)) return KeyboardEvent.prototype;
	if (/click|^mouse|^contextmenu/.test(type)) return MouseEvent.prototype;
	if (type === 'focus' || type === 'blur') return FocusEvent.prototype;
	return Event.prototype;
}

function getInline(target, type) {
	if (!isNode(target) || target.nodeType !== 1) return null;
	const src = N.getAttr(target, 'on' + type);
	if (src === null || src === undefined) return null;
	let m = INL.get(target);
	if (!m) { m = new Map(); INL.set(target, m); }
	const c = m.get(type);
	if (c && c.src === src) return c.fn;
	let fn = null;
	try { fn = new Function('event', src); }
	catch (e) { console.error('inline handler on' + type + ': ' + e); }
	m.set(type, { src, fn });
	return fn;
}

function callListener(target, fn, ev) {
	try {
		if (typeof fn === 'function') return fn.call(target, ev);
		if (fn && typeof fn.handleEvent === 'function') return fn.handleEvent(ev);
	} catch (e) {
		reportError(e);
	}
	return undefined;
}

function reportError(e) {
	let s;
	if (e instanceof Error) s = e.name + ': ' + e.message + (e.stack ? '\n' + e.stack : '');
	else s = 'Uncaught ' + fmt1(e);
	N.log(4, s);
	try {
		const h = HND.get(winTarget) && HND.get(winTarget).get('error');
		if (typeof h === 'function') h.call(G, String(e && e.message || e), '', 0, 0, e);
	} catch (x) { /* ignore */ }
}

/* run the listeners of one target for one phase (1 capture, 2 target, 3 bubble) */
function runListeners(target, ev, phase, type) {
	const m = LST.get(target);
	const list = m && m.get(type);
	if (list && list.length) {
		for (const l of list.slice()) {
			if (l.removed) continue;
			if (phase === 1 && !l.capture) continue;
			if (phase === 3 && l.capture) continue;
			if (l.once) removeL(target, type, l.fn, l.capture);
			callListener(target, l.fn, ev);
			if (ev.__stopi) return;
		}
	}
	if (phase !== 1) {
		const hm = HND.get(target);
		let h = hm && hm.get(type);
		if (h === undefined) h = getInline(target, type);
		if (typeof h === 'function') {
			const r = callListener(target, h, ev);
			if (r === false) ev.preventDefault();
		}
	}
}

function removeL(target, type, fn, capture) {
	const m = LST.get(target);
	const list = m && m.get(type);
	if (!list) return;
	for (let i = 0; i < list.length; i++) {
		if (list[i].fn === fn && list[i].capture === capture) {
			list[i].removed = true;
			list.splice(i, 1);
			return;
		}
	}
}

const ETP = {
	addEventListener(type, fn, opts) {
		if (fn === null || fn === undefined) return;
		type = String(type);
		const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
		const once = !!(opts && typeof opts === 'object' && opts.once);
		const target = this === undefined ? G : this;
		let m = LST.get(target);
		if (!m) { m = new Map(); LST.set(target, m); }
		let list = m.get(type);
		if (!list) { list = []; m.set(type, list); }
		for (const l of list) if (l.fn === fn && l.capture === capture) return;
		list.push({ fn, capture, once });
		ensureListen(target, type);
		if (opts && typeof opts === 'object' && opts.signal && opts.signal.addEventListener) {
			opts.signal.addEventListener('abort', () => removeL(target, type, fn, capture));
		}
	},
	removeEventListener(type, fn, opts) {
		const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
		removeL(this === undefined ? G : this, String(type), fn, capture);
	},
	dispatchEvent(ev) {
		const target = this === undefined ? G : this;
		jsDispatch(target, ev);
		return !ev.defaultPrevented;
	},
};

function eventPath(target) {
	const path = [];
	if (isNode(target)) {
		let n = N.parent(target);
		let top = target;
		while (n) { path.push(n); top = n; n = N.parent(n); }
		if (top.nodeType === 9) path.push(winTarget);
	}
	return path; /* ancestors, nearest first */
}

function jsDispatch(target, ev) {
	const type = ev.type;
	const path = eventPath(target);
	val(ev, '__target', target);
	val(ev, '__stop', false);
	val(ev, '__stopi', false);
	try {
		for (let i = path.length - 1; i >= 0 && !ev.__stop; i--) {
			val(ev, '__cur', path[i]); val(ev, '__phase', 1);
			runListeners(path[i], ev, 1, type);
		}
		if (!ev.__stop) {
			val(ev, '__cur', target); val(ev, '__phase', 2);
			runListeners(target, ev, 2, type);
		}
		if (ev.bubbles) {
			for (let i = 0; i < path.length && !ev.__stop; i++) {
				val(ev, '__cur', path[i]); val(ev, '__phase', 3);
				runListeners(path[i], ev, 3, type);
			}
		}
	} finally {
		val(ev, '__cur', null);
		val(ev, '__phase', 0);
	}
}

/* hook: libdom delivered an event at currentTarget */
function hookDispatch(cur, ev, phase) {
	const type = ev.type;
	if (type === 'DOMContentLoaded') { docState = 'interactive'; if (ceCount && phase !== 3 && cur && N.nodeType(cur) === 9) ceScan(cur); }
	runListeners(cur, ev, phase, type);
	/* the window is not a libdom node: run its listeners at the document */
	if (cur && cur.nodeType === 9 && phase !== 1 && !ev.__stop && ev.bubbles &&
	    type !== 'DOMContentLoaded' && type !== 'load') {
		val(ev, '__cur', winTarget); val(ev, '__phase', 3);
		try { runListeners(winTarget, ev, 3, type); }
		finally { delete ev.__cur; delete ev.__phase; }
	}
}

/* hook: window-level lifecycle events */
function hookWindow(type, ev) {
	if (!ev) ev = new Event(type);
	if ((type === 'DOMContentLoaded' || type === 'load') && ceCount && G.document) ceScan(G.document);
	if (type === 'DOMContentLoaded') docState = 'interactive';
	if (type === 'load') docState = 'complete';
	val(ev, '__cur', winTarget); val(ev, '__phase', 2);
	if (type === 'load' && ev.__target === undefined) val(ev, '__target', G.document);
	const hm = HND.get(winTarget);
	if (type === 'load' && !(hm && hm.has('load'))) {
		/* <body onload> is window.onload */
		const b = G.document && G.document.body;
		const fn = b ? getInline(b, 'load') : null;
		runListeners(winTarget, ev, 2, type);
		if (fn) { const r = callListener(G, fn, ev); if (r === false) ev.preventDefault(); }
	} else {
		runListeners(winTarget, ev, 2, type);
	}
	delete ev.__cur; delete ev.__phase;
}

const ON_EVENTS = ['click', 'dblclick', 'mousedown', 'mouseup', 'mouseover', 'mouseout',
	'mousemove', 'mouseenter', 'mouseleave', 'contextmenu', 'wheel', 'keydown', 'keyup', 'keypress',
	'input', 'change', 'submit', 'reset', 'load', 'unload', 'beforeunload', 'error', 'focus',
	'blur', 'scroll', 'resize', 'select', 'abort', 'animationend', 'animationstart',
	'transitionend', 'pointerdown', 'pointerup', 'pointermove', 'touchstart', 'touchend',
	'touchmove', 'hashchange', 'popstate', 'message', 'storage', 'toggle', 'invalid',
	'pageshow', 'pagehide', 'readystatechange', 'visibilitychange', 'online', 'offline'];

function defineOn(proto) {
	for (const t of ON_EVENTS) {
		def(proto, 'on' + t, function () {
			const target = this === undefined ? G : this;
			const hm = HND.get(target);
			if (hm && hm.has(t)) return hm.get(t);
			return getInline(target, t);
		}, function (fn) {
			const target = this === undefined ? G : this;
			let hm = HND.get(target);
			if (!hm) { hm = new Map(); HND.set(target, hm); }
			hm.set(t, typeof fn === 'function' ? fn : null);
			if (typeof fn === 'function') ensureListen(target, t);
		});
	}
}

/* ---------------- DOM classes ---------------- */
function illegal() { throw new TypeError('Illegal constructor'); }
function EventTarget() { if (!new.target) illegal(); }
Object.assign(EventTarget.prototype, ETP);
function Node() { illegal(); }
Node.prototype = Object.create(EventTarget.prototype);
val(Node.prototype, 'constructor', Node);
Object.assign(Node, { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4,
	PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10,
	DOCUMENT_FRAGMENT_NODE: 11, DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2,
	DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16 });
defineOn(Node.prototype);

function sub(name, parent, ctor) {
	const C = ctor || function () { illegal(); };
	Object.defineProperty(C, 'name', { value: name });
	C.prototype = Object.create(parent.prototype);
	val(C.prototype, 'constructor', C);
	Object.setPrototypeOf(C, parent);
	G[name] = C;
	return C;
}

function childArray(n, elementsOnly) {
	const out = [];
	for (let c = N.first(n); c; c = N.next(c))
		if (!elementsOnly || N.nodeType(c) === 1) out.push(c);
	return out;
}
function makeList(arr) {
	val(arr, 'item', function (i) { return this[i] === undefined ? null : this[i]; });
	val(arr, 'namedItem', function (k) { return this.find((e) => e.id === k || e.getAttribute('name') === k) || null; });
	return arr;
}
function toNode(x) { return isNode(x) ? x : N.createText(String(x)); }

const NP = Node.prototype;
def(NP, 'nodeType', function () { return N.nodeType(this); });
def(NP, 'nodeName', function () {
	const t = N.nodeType(this);
	if (t === 1) return String(N.nodeName(this)).toUpperCase();
	if (t === 3) return '#text';
	if (t === 8) return '#comment';
	if (t === 9) return '#document';
	if (t === 11) return '#document-fragment';
	return N.nodeName(this);
});
def(NP, 'parentNode', function () { return N.parent(this); });
def(NP, 'parentElement', function () { const p = N.parent(this); return p && N.nodeType(p) === 1 ? p : null; });
def(NP, 'firstChild', function () { return N.first(this); });
def(NP, 'lastChild', function () { return N.last(this); });
def(NP, 'nextSibling', function () { return N.next(this); });
def(NP, 'previousSibling', function () { return N.prev(this); });
def(NP, 'ownerDocument', function () { return N.nodeType(this) === 9 ? null : N.ownerDoc(this); });
def(NP, 'childNodes', function () { return makeList(childArray(this, false)); });
def(NP, 'isConnected', function () {
	let n = this, p;
	while ((p = N.parent(n))) n = p;
	return N.nodeType(n) === 9;
});
def(NP, 'nodeValue', function () { return N.nodeValue(this); }, function (v) {
	const t = N.nodeType(this); if (t === 3 || t === 8) N.setValue(this, v == null ? '' : String(v));
});
def(NP, 'textContent', function () {
	const t = N.nodeType(this);
	if (t === 9) return null;
	const s = N.text(this);
	return s === null ? '' : s;
}, function (v) {
	N.setText(this, v == null ? '' : String(v));
});
def(NP, 'baseURI', function () { return N.baseURL(); });
NP.hasChildNodes = function () { return N.first(this) !== null; };
NP.appendChild = function (c) { if (!isNode(c)) throw new TypeError('appendChild: not a Node'); N.append(this, c); return c; };
NP.insertBefore = function (c, ref) {
	if (!isNode(c)) throw new TypeError('insertBefore: not a Node');
	if (ref) N.insertBefore(this, c, ref); else N.append(this, c);
	return c;
};
NP.removeChild = function (c) { N.remove(this, c); return c; };
NP.replaceChild = function (nc, old) { N.replace(this, nc, old); return old; };
NP.cloneNode = function (deep) { const c = N.clone(this, !!deep); if (ceCount) ceScan(c); return c; };
NP.contains = function (o) {
	for (let n = o; n; n = N.parent(n)) if (n === this) return true;
	return false;
};
NP.getRootNode = function () { let n = this, p; while ((p = N.parent(n))) n = p; return n; };
NP.isSameNode = function (o) { return o === this; };
NP.isEqualNode = function (o) { return isNode(o) && o.nodeType === this.nodeType && serialize(o) === serialize(this); };
NP.normalize = function () {};
/* Ancestor chains + one sibling scan under the common ancestor: was two
 * whole-tree walks per call (jQuery's uniqueSort made that quadratic). */
NP.compareDocumentPosition = function (o) {
	if (o === this) return 0;
	if (!isNode(o)) throw new TypeError('compareDocumentPosition: not a Node');
	const pa = [], pb = [];
	for (let n = this; n; n = N.parent(n)) pa.push(n);
	for (let n = o; n; n = N.parent(n)) pb.push(n);
	let i = pa.length - 1, j = pb.length - 1;
	if (pa[i] !== pb[j]) return 1;
	while (i >= 0 && j >= 0 && pa[i] === pb[j]) { i--; j--; }
	if (i < 0) return 20;
	if (j < 0) return 10;
	const b = pb[j];
	for (let x = N.next(pa[i]); x; x = N.next(x)) if (x === b) return 4;
	return 2;
};
/* ChildNode / ParentNode mixins */
const CNM = {
	remove() { const p = N.parent(this); if (p) N.remove(p, this); },
	before(...nodes) { const p = N.parent(this); if (!p) return; for (const x of nodes) N.insertBefore(p, toNode(x), this); },
	after(...nodes) {
		const p = N.parent(this); if (!p) return;
		const ref = N.next(this);
		for (const x of nodes) { if (ref) N.insertBefore(p, toNode(x), ref); else N.append(p, toNode(x)); }
	},
	replaceWith(...nodes) { const p = N.parent(this); if (!p) return; this.before(...nodes); N.remove(p, this); },
};
const PNM = {
	append(...nodes) { for (const x of nodes) N.append(this, toNode(x)); },
	prepend(...nodes) {
		const f = N.first(this);
		for (const x of nodes) { if (f) N.insertBefore(this, toNode(x), f); else N.append(this, toNode(x)); }
	},
	replaceChildren(...nodes) { let c; while ((c = N.first(this))) N.remove(this, c); this.append(...nodes); },
	querySelector(sel) { return qsa(this, sel, true)[0] || null; },
	querySelectorAll(sel) { return makeList(qsa(this, sel, false)); },
	getElementsByTagName(tag) {
		tag = String(tag).toLowerCase();
		return makeList(N.collect(this, tag === '*' ? null : tag, null, null, false));
	},
	getElementsByClassName(cls) {
		const want = String(cls).trim().split(/\s+/).filter(Boolean);
		return makeList(want.length ? N.collect(this, null, null, want, false) : []);
	},
};
const PNG = {
	children() { return makeList(childArray(this, true)); },
	childElementCount() { return childArray(this, true).length; },
	firstElementChild() { for (let c = N.first(this); c; c = N.next(c)) if (N.nodeType(c) === 1) return c; return null; },
	lastElementChild() { for (let c = N.last(this); c; c = N.prev(c)) if (N.nodeType(c) === 1) return c; return null; },
};
const CNG = {
	nextElementSibling() { for (let c = N.next(this); c; c = N.next(c)) if (N.nodeType(c) === 1) return c; return null; },
	previousElementSibling() { for (let c = N.prev(this); c; c = N.prev(c)) if (N.nodeType(c) === 1) return c; return null; },
};
function mixin(proto, methods, getters) {
	if (methods) for (const k of Object.keys(methods)) val(proto, k, methods[k]);
	if (getters) for (const k of Object.keys(getters)) def(proto, k, getters[k]);
}

/* walk nodes in document order; f returns true to stop */
function walk(root, f, skipRoot) {
	if (!skipRoot && f(root)) return true;
	let n = N.first(root);
	while (n) {
		if (f(n)) return true;
		const c = N.first(n);
		if (c) { n = c; continue; }
		for (;;) {
			if (n === root) return false;
			const s = N.next(n);
			if (s) { n = s; break; }
			n = N.parent(n);
			if (!n || n === root) return false;
		}
	}
	return false;
}
function lname(n) { return String(N.nodeName(n)).toLowerCase(); }
function classesOf(n) { const c = N.getAttr(n, 'class'); return c ? c.trim().split(/\s+/) : []; }

const CharacterData = sub('CharacterData', Node);
mixin(CharacterData.prototype, CNM, CNG);
def(CharacterData.prototype, 'data', function () { return N.nodeValue(this) || ''; }, function (v) { N.setValue(this, String(v)); });
def(CharacterData.prototype, 'length', function () { return (N.nodeValue(this) || '').length; });
CharacterData.prototype.appendData = function (s) { this.data = this.data + s; };
const Text = sub('Text', CharacterData, function Text(s) { return N.createText(s === undefined ? '' : String(s)); });
def(Text.prototype, 'wholeText', function () { return this.data; });
const Comment = sub('Comment', CharacterData);

const DocumentFragment = sub('DocumentFragment', Node, function DocumentFragment() { return N.createFragment(); });
mixin(DocumentFragment.prototype, PNM, PNG);
DocumentFragment.prototype.getElementById = function (id) { return qsa(this, '#' + cssEscape(id), true)[0] || null; };

const Element = sub('Element', Node);
const EP = Element.prototype;
mixin(EP, Object.assign({}, CNM, PNM), Object.assign({}, CNG, PNG));
def(EP, 'tagName', function () { return String(N.nodeName(this)).toUpperCase(); });
def(EP, 'localName', function () { return lname(this); });
def(EP, 'namespaceURI', function () { return 'http://www.w3.org/1999/xhtml'; });
EP.getAttribute = function (n) { const v = N.getAttr(this, String(n).toLowerCase()); return v === undefined ? null : v; };
EP.setAttribute = function (n, v) { n = String(n).toLowerCase(); N.setAttr(this, n, String(v)); if (n.startsWith('on')) ensureListen(this, n.slice(2)); };
EP.removeAttribute = function (n) { N.removeAttr(this, String(n).toLowerCase()); };
EP.hasAttribute = function (n) { return N.hasAttr(this, String(n).toLowerCase()); };
EP.hasAttributes = function () { return N.attrs(this).length > 0; };
EP.toggleAttribute = function (n, force) {
	const has = this.hasAttribute(n);
	if (force === true || (force === undefined && !has)) { if (!has) this.setAttribute(n, ''); return true; }
	if (has) this.removeAttribute(n);
	return false;
};
EP.getAttributeNames = function () { return N.attrs(this).map((p) => p[0]); };
EP.getAttributeNS = function (ns, n) { return this.getAttribute(n); };
EP.setAttributeNS = function (ns, n, v) { this.setAttribute(String(n).replace(/^.*:/, ''), v); };
EP.removeAttributeNS = function (ns, n) { this.removeAttribute(n); };
EP.hasAttributeNS = function (ns, n) { return this.hasAttribute(n); };
def(EP, 'attributes', function () {
	const el = this;
	const arr = N.attrs(this).map((p) => ({ name: p[0], localName: p[0], value: p[1], nodeName: p[0], nodeValue: p[1], ownerElement: el }));
	val(arr, 'getNamedItem', (k) => arr.find((a) => a.name === String(k).toLowerCase()) || null);
	val(arr, 'item', (i) => arr[i] || null);
	return arr;
});
def(EP, 'id', function () { return this.getAttribute('id') || ''; }, function (v) { this.setAttribute('id', v); });
def(EP, 'className', function () { return this.getAttribute('class') || ''; }, function (v) { this.setAttribute('class', v); });
EP.matches = function (sel) { return matchesSel(this, parseSel(sel)); };
EP.webkitMatchesSelector = EP.msMatchesSelector = EP.matches;
EP.closest = function (sel) {
	const s = parseSel(sel);
	for (let n = this; n && N.nodeType(n) === 1; n = N.parent(n)) if (matchesSel(n, s)) return n;
	return null;
};
const zeroRect = () => ({ x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0, toJSON() { return this; } });
EP.getBoundingClientRect = zeroRect;
EP.getClientRects = function () { return []; };
for (const k of ['scrollIntoView', 'scrollTo', 'scrollBy', 'scroll', 'focus', 'blur', 'setPointerCapture',
	'releasePointerCapture', 'requestFullscreen'])
	EP[k] = function () {};
EP.animate = function () { return { cancel() {}, finish() {}, play() {}, pause() {}, finished: Promise.resolve(), onfinish: null }; };
for (const k of ['scrollTop', 'scrollLeft']) def(EP, k, function () { return 0; }, function () {});
for (const k of ['scrollWidth', 'scrollHeight', 'clientWidth', 'clientHeight', 'clientTop', 'clientLeft']) {
	def(EP, k, function () {
		const d0 = G.document;
		if (this === d0.documentElement || this === d0.body) {
			const d = N.dims();
			if (/Width$/.test(k)) return d[0];
			if (/Height$/.test(k)) return d[1];
		}
		return 0;
	});
}
def(EP, 'innerHTML', function () { return serializeChildren(this); }, function (v) {
	let c;
	while ((c = N.first(this))) N.remove(this, c);
	const tag = lname(this);
	v = v == null ? '' : String(v);
	if (tag === 'script' || tag === 'style' || tag === 'textarea') { if (v) N.append(this, N.createText(v)); return; }
	parseHTMLInto(this, v);
});
def(EP, 'outerHTML', function () { return serialize(this); }, function (v) {
	const p = N.parent(this);
	if (!p) return;
	const f = N.createFragment();
	parseHTMLInto(f, String(v));
	N.insertBefore(p, f, this);
	N.remove(p, this);
});
EP.insertAdjacentHTML = function (pos, html) {
	const f = N.createFragment();
	parseHTMLInto(f, String(html));
	insertAdj(this, pos, f);
};
EP.insertAdjacentElement = function (pos, el) { insertAdj(this, pos, el); return el; };
EP.insertAdjacentText = function (pos, t) { insertAdj(this, pos, N.createText(String(t))); };
function insertAdj(el, pos, node) {
	pos = String(pos).toLowerCase();
	const p = N.parent(el);
	if (pos === 'beforebegin') { if (p) N.insertBefore(p, node, el); }
	else if (pos === 'afterbegin') { const f = N.first(el); if (f) N.insertBefore(el, node, f); else N.append(el, node); }
	else if (pos === 'beforeend') N.append(el, node);
	else if (pos === 'afterend') { if (p) { const n = N.next(el); if (n) N.insertBefore(p, node, n); else N.append(p, node); } }
}
const CLS = new WeakMap();
def(EP, 'classList', function () {
	let l = CLS.get(this);
	if (!l) { l = new DOMTokenList(this, 'class'); CLS.set(this, l); }
	return l;
}, function (v) { this.setAttribute('class', v); });
EP.getElementById = function (id) { return qsa(this, '#' + cssEscape(id), true)[0] || null; };

class DOMTokenList {
	constructor(el, attr) { val(this, '__el', el); val(this, '__attr', attr); }
	__get() { const v = N.getAttr(this.__el, this.__attr); return v ? v.trim().split(/\s+/).filter(Boolean) : []; }
	__set(list) { N.setAttr(this.__el, this.__attr, list.join(' ')); }
	get length() { return this.__get().length; }
	get value() { return N.getAttr(this.__el, this.__attr) || ''; }
	set value(v) { N.setAttr(this.__el, this.__attr, String(v)); }
	item(i) { const l = this.__get(); return l[i] === undefined ? null : l[i]; }
	contains(t) { return this.__get().includes(String(t)); }
	add(...ts) {
		const l = this.__get(); let ch = false;
		for (const t of ts) if (!l.includes(String(t))) { l.push(String(t)); ch = true; }
		if (ch) this.__set(l);
	}
	remove(...ts) { const l = this.__get(); const rm = ts.map(String); const n = l.filter((x) => !rm.includes(x)); if (n.length !== l.length) this.__set(n); }
	toggle(t, force) {
		t = String(t);
		const has = this.contains(t);
		if (force === true || (force === undefined && !has)) { if (!has) this.add(t); return true; }
		if (has) this.remove(t);
		return false;
	}
	replace(a, b) { const l = this.__get(); const i = l.indexOf(String(a)); if (i < 0) return false; l[i] = String(b); this.__set(l); return true; }
	supports() { return true; }
	forEach(f, thisArg) { this.__get().forEach((v, i) => f.call(thisArg, v, i, this)); }
	entries() { return this.__get().entries(); }
	keys() { return this.__get().keys(); }
	values() { return this.__get().values(); }
	[Symbol.iterator]() { return this.__get()[Symbol.iterator](); }
	toString() { return this.value; }
}
G.DOMTokenList = DOMTokenList;

/* ---- inline style ---- */
const camelToKebab = (s) => s.startsWith('--') ? s : s.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase()).replace(/^(webkit|moz|ms)-/, '-$1-');
const kebabToCamel = (s) => s.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
function parseStyle(s) {
	const m = new Map();
	if (!s) return m;
	for (const decl of s.split(';')) {
		const i = decl.indexOf(':');
		if (i < 0) continue;
		const k = decl.slice(0, i).trim().toLowerCase();
		const v = decl.slice(i + 1).trim();
		if (k) m.set(k, v);
	}
	return m;
}
function writeStyle(el, m) {
	const s = [...m].map(([k, v]) => k + ': ' + v).join('; ');
	N.setAttr(el, 'style', s);
}
const STY = new WeakMap();
function styleFor(el, readonly) {
	const methods = {
		getPropertyValue: (k) => parseStyle(N.getAttr(el, 'style')).get(String(k).toLowerCase()) || '',
		getPropertyPriority: () => '',
		setProperty: (k, v) => {
			if (readonly) return;
			const m = parseStyle(N.getAttr(el, 'style'));
			k = String(k).toLowerCase();
			if (v === null || v === undefined || v === '') m.delete(k); else m.set(k, String(v));
			writeStyle(el, m);
		},
		removeProperty: (k) => {
			if (readonly) return '';
			const m = parseStyle(N.getAttr(el, 'style'));
			k = String(k).toLowerCase();
			const o = m.get(k) || '';
			m.delete(k); writeStyle(el, m);
			return o;
		},
		item: (i) => [...parseStyle(N.getAttr(el, 'style')).keys()][i] || '',
	};
	return new Proxy({}, {
		get(_, p) {
			if (typeof p === 'symbol') return undefined;
			if (p in methods) return methods[p];
			if (p === 'cssText') return N.getAttr(el, 'style') || '';
			if (p === 'length') return parseStyle(N.getAttr(el, 'style')).size;
			if (p === 'cssFloat') p = 'float';
			const v = parseStyle(N.getAttr(el, 'style')).get(camelToKebab(p));
			return v === undefined ? '' : v;
		},
		set(_, p, v) {
			if (readonly || typeof p === 'symbol') return true;
			if (p === 'cssText') { N.setAttr(el, 'style', String(v)); return true; }
			if (p === 'cssFloat') p = 'float';
			methods.setProperty(camelToKebab(p), v);
			return true;
		},
		has(_, p) { return typeof p === 'string'; },
	});
}
function styleGetter() {
	let s = STY.get(this);
	if (!s) { s = styleFor(this, false); STY.set(this, s); }
	return s;
}

/* ---- dataset ---- */
const DS = new WeakMap();
function datasetGetter() {
	let d = DS.get(this);
	if (d) return d;
	const el = this;
	d = new Proxy({}, {
		get(_, p) { if (typeof p !== 'string') return undefined; const v = N.getAttr(el, 'data-' + camelToKebab(p)); return v === null ? undefined : v; },
		set(_, p, v) { N.setAttr(el, 'data-' + camelToKebab(p), String(v)); return true; },
		deleteProperty(_, p) { N.removeAttr(el, 'data-' + camelToKebab(p)); return true; },
		has(_, p) { return N.hasAttr(el, 'data-' + camelToKebab(p)); },
		ownKeys() { return N.attrs(el).filter((a) => a[0].startsWith('data-')).map((a) => kebabToCamel(a[0].slice(5))); },
		getOwnPropertyDescriptor(_, p) { const v = N.getAttr(el, 'data-' + camelToKebab(p)); return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: true }; },
	});
	DS.set(this, d);
	return d;
}

const HTMLElement = sub('HTMLElement', Element, HTMLElementCtor);
const HP = HTMLElement.prototype;
def(HP, 'style', styleGetter, function (v) { N.setAttr(this, 'style', String(v)); });
def(HP, 'dataset', datasetGetter);
def(HP, 'innerText', function () { return this.textContent; }, function (v) { this.textContent = v; });
def(HP, 'outerText', function () { return this.textContent; });
def(HP, 'hidden', function () { return this.hasAttribute('hidden'); }, function (v) { this.toggleAttribute('hidden', !!v); });
def(HP, 'offsetParent', function () { return N.parent(this); });
for (const k of ['offsetWidth', 'offsetHeight', 'offsetTop', 'offsetLeft']) def(HP, k, function () { return 0; });
def(HP, 'isContentEditable', function () { return false; });
HP.click = function () {
	if (this.disabled) return;
	const ev = new MouseEvent('click', { bubbles: true, cancelable: true, button: 0 });
	jsDispatch(this, ev);
	if (!ev.defaultPrevented) {
		const t = lname(this);
		if (t === 'a' && this.hasAttribute('href')) N.navigate(this.getAttribute('href'));
		else if ((t === 'input' || t === 'button') && this.type === 'submit' && this.form) this.form.requestSubmit();
		else if (t === 'input' && (this.type === 'checkbox' || this.type === 'radio')) this.checked = this.type === 'radio' ? true : !this.checked;
	}
};

function reflect(proto, prop, attr, kind) {
	attr = attr || prop.toLowerCase();
	if (kind === 'bool') {
		def(proto, prop, function () { return N.hasAttr(this, attr); }, function (v) { if (v) N.setAttr(this, attr, ''); else N.removeAttr(this, attr); });
	} else if (kind === 'url') {
		def(proto, prop, function () { const v = N.getAttr(this, attr); return v === null ? '' : (N.resolve(v) || v); }, function (v) { N.setAttr(this, attr, String(v)); });
	} else if (kind === 'int') {
		def(proto, prop, function () { const v = parseInt(N.getAttr(this, attr)); return isNaN(v) ? 0 : v; }, function (v) { N.setAttr(this, attr, String(v | 0)); });
	} else {
		def(proto, prop, function () { const v = N.getAttr(this, attr); return v === null ? '' : v; }, function (v) { N.setAttr(this, attr, String(v)); });
	}
}
for (const p of ['title', 'lang', 'dir', 'accessKey', 'slot']) reflect(HP, p);
def(HP, 'tabIndex', function () { const v = parseInt(N.getAttr(this, 'tabindex')); return isNaN(v) ? -1 : v; }, function (v) { N.setAttr(this, 'tabindex', String(v)); });
def(HP, 'contentEditable', function () { return N.getAttr(this, 'contenteditable') || 'inherit'; }, function (v) { N.setAttr(this, 'contenteditable', String(v)); });
def(HP, 'draggable', function () { return N.getAttr(this, 'draggable') === 'true'; }, function (v) { N.setAttr(this, 'draggable', String(!!v)); });

const ELPROTO = Object.create(null);
function htmlClass(name, tags, setup) {
	const C = sub(name, HTMLElement);
	for (const t of tags) ELPROTO[t] = C.prototype;
	if (setup) setup(C.prototype);
	return C;
}
function formOf(el) { for (let n = N.parent(el); n; n = N.parent(n)) if (N.nodeType(n) === 1 && lname(n) === 'form') return n; return null; }
function formControl(P) {
	reflect(P, 'name'); reflect(P, 'disabled', 'disabled', 'bool'); reflect(P, 'required', 'required', 'bool');
	reflect(P, 'autofocus', 'autofocus', 'bool');
	def(P, 'form', function () { return formOf(this); });
	def(P, 'value', function () {
		const v = N.getValue(this);
		if (v !== undefined && v !== null) return v;
		const a = N.getAttr(this, 'value'); return a === null ? '' : a;
	}, function (v) {
		v = v == null ? '' : String(v);
		if (!N.setFormValue(this, v)) N.setAttr(this, 'value', v);
	});
	def(P, 'validity', function () { return { valid: true, valueMissing: false }; });
	def(P, 'validationMessage', function () { return ''; });
	def(P, 'willValidate', function () { return true; });
	P.checkValidity = P.reportValidity = function () { return true; };
	P.setCustomValidity = function () {};
	P.focus = function () { G.document.__active = this; };
	P.blur = function () { if (G.document.__active === this) G.document.__active = null; };
	P.select = function () {};
	P.setSelectionRange = function () {};
	def(P, 'labels', function () { return []; });
}
htmlClass('HTMLInputElement', ['input'], (P) => {
	formControl(P);
	def(P, 'type', function () { return (N.getAttr(this, 'type') || 'text').toLowerCase(); }, function (v) { N.setAttr(this, 'type', String(v)); });
	for (const p of ['placeholder', 'pattern', 'accept', 'alt', 'autocomplete', 'min', 'max', 'step', 'inputMode']) reflect(P, p);
	reflect(P, 'readOnly', 'readonly', 'bool'); reflect(P, 'multiple', 'multiple', 'bool');
	reflect(P, 'src', 'src', 'url');
	reflect(P, 'defaultValue', 'value');
	def(P, 'maxLength', function () { const v = parseInt(N.getAttr(this, 'maxlength')); return isNaN(v) ? -1 : v; }, function (v) { N.setAttr(this, 'maxlength', String(v)); });
	reflect(P, 'size', 'size', 'int');
	def(P, 'checked', function () { return N.checked(this); }, function (v) { N.checked(this, !!v); });
	reflect(P, 'defaultChecked', 'checked', 'bool');
	def(P, 'valueAsNumber', function () { return parseFloat(this.value); }, function (v) { this.value = String(v); });
	def(P, 'selectionStart', function () { return this.value.length; }, function () {});
	def(P, 'selectionEnd', function () { return this.value.length; }, function () {});
	def(P, 'files', function () { return []; });
});
htmlClass('HTMLTextAreaElement', ['textarea'], (P) => {
	formControl(P);
	reflect(P, 'placeholder'); reflect(P, 'readOnly', 'readonly', 'bool');
	reflect(P, 'rows', 'rows', 'int'); reflect(P, 'cols', 'cols', 'int');
	def(P, 'type', function () { return 'textarea'; });
	def(P, 'defaultValue', function () { return this.textContent; }, function (v) { this.textContent = v; });
});
htmlClass('HTMLSelectElement', ['select'], (P) => {
	formControl(P);
	reflect(P, 'multiple', 'multiple', 'bool');
	def(P, 'type', function () { return this.multiple ? 'select-multiple' : 'select-one'; });
	def(P, 'options', function () { return makeList(qsa(this, 'option', false)); });
	def(P, 'length', function () { return this.options.length; });
	def(P, 'selectedIndex', function () {
		const o = this.options; for (let i = 0; i < o.length; i++) if (o[i].selected) return i;
		return o.length ? 0 : -1;
	}, function (i) { const o = this.options; for (let k = 0; k < o.length; k++) o[k].selected = (k === i); });
	def(P, 'selectedOptions', function () { return makeList(this.options.filter((o) => o.selected)); });
	P.add = function (o, before) { if (before) this.insertBefore(o, before); else this.appendChild(o); };
	P.item = function (i) { return this.options[i] || null; };
});
htmlClass('HTMLOptionElement', ['option'], (P) => {
	reflect(P, 'disabled', 'disabled', 'bool'); reflect(P, 'label');
	reflect(P, 'defaultSelected', 'selected', 'bool');
	def(P, 'selected', function () { return N.hasAttr(this, 'selected'); }, function (v) { if (v) N.setAttr(this, 'selected', ''); else N.removeAttr(this, 'selected'); });
	def(P, 'value', function () { const v = N.getAttr(this, 'value'); return v === null ? this.textContent.trim() : v; }, function (v) { N.setAttr(this, 'value', String(v)); });
	def(P, 'text', function () { return this.textContent; }, function (v) { this.textContent = v; });
	def(P, 'index', function () { const s = this.closest('select'); return s ? s.options.indexOf(this) : 0; });
});
G.Option = function Option(text, value, ds, sel) {
	const o = G.document.createElement('option');
	if (text !== undefined) o.text = text;
	if (value !== undefined) o.value = value;
	if (ds || sel) o.selected = true;
	return o;
};
htmlClass('HTMLButtonElement', ['button'], (P) => {
	formControl(P);
	def(P, 'type', function () { return (N.getAttr(this, 'type') || 'submit').toLowerCase(); }, function (v) { N.setAttr(this, 'type', String(v)); });
	def(P, 'value', function () { return N.getAttr(this, 'value') || ''; }, function (v) { N.setAttr(this, 'value', String(v)); });
});
htmlClass('HTMLFormElement', ['form'], (P) => {
	reflect(P, 'name'); reflect(P, 'action', 'action', 'url'); reflect(P, 'target');
	reflect(P, 'enctype'); reflect(P, 'acceptCharset', 'accept-charset');
	reflect(P, 'noValidate', 'novalidate', 'bool');
	def(P, 'method', function () { return (N.getAttr(this, 'method') || 'get').toLowerCase(); }, function (v) { N.setAttr(this, 'method', String(v)); });
	def(P, 'elements', function () { return makeList(qsa(this, 'input,select,textarea,button', false)); });
	def(P, 'length', function () { return this.elements.length; });
	P.submit = function () {
		const fd = new FormData(this);
		const action = this.action || N.href();
		if (this.method === 'post') console.warn('form.submit(): POST from script not supported, using GET');
		const q = new URLSearchParams(fd).toString();
		N.navigate(action.split('?')[0] + (q ? '?' + q : ''));
	};
	P.requestSubmit = function () {
		const ev = new Event('submit', { bubbles: true, cancelable: true });
		jsDispatch(this, ev);
		if (!ev.defaultPrevented) this.submit();
	};
	P.reset = function () {};
	P.checkValidity = P.reportValidity = function () { return true; };
});
htmlClass('HTMLAnchorElement', ['a'], (P) => {
	reflect(P, 'href', 'href', 'url');
	for (const p of ['target', 'rel', 'download', 'hreflang', 'type', 'name']) reflect(P, p);
	def(P, 'text', function () { return this.textContent; }, function (v) { this.textContent = v; });
	urlParts(P, function () { return this.href; }, function (v) { this.href = v; });
	P.toString = function () { return this.href; };
});
htmlClass('HTMLAreaElement', ['area'], (P) => { reflect(P, 'href', 'href', 'url'); reflect(P, 'alt'); });
htmlClass('HTMLImageElement', ['img'], (P) => {
	reflect(P, 'src', 'src', 'url'); reflect(P, 'alt'); reflect(P, 'srcset'); reflect(P, 'sizes');
	reflect(P, 'crossOrigin', 'crossorigin'); reflect(P, 'loading'); reflect(P, 'decoding');
	reflect(P, 'useMap', 'usemap'); reflect(P, 'isMap', 'ismap', 'bool');
	def(P, 'width', function () { return parseInt(N.getAttr(this, 'width')) || 0; }, function (v) { N.setAttr(this, 'width', String(v)); });
	def(P, 'height', function () { return parseInt(N.getAttr(this, 'height')) || 0; }, function (v) { N.setAttr(this, 'height', String(v)); });
	def(P, 'naturalWidth', function () { return this.width; });
	def(P, 'naturalHeight', function () { return this.height; });
	def(P, 'complete', function () { return true; });
	def(P, 'currentSrc', function () { return this.src; });
	P.decode = function () { return Promise.resolve(); };
});
G.Image = function Image(w, h) {
	const i = G.document.createElement('img');
	if (w !== undefined) i.width = w;
	if (h !== undefined) i.height = h;
	return i;
};
htmlClass('HTMLScriptElement', ['script'], (P) => {
	reflect(P, 'src', 'src', 'url'); reflect(P, 'type'); reflect(P, 'charset');
	reflect(P, 'async', 'async', 'bool'); reflect(P, 'defer', 'defer', 'bool');
	reflect(P, 'crossOrigin', 'crossorigin'); reflect(P, 'integrity'); reflect(P, 'noModule', 'nomodule', 'bool');
	def(P, 'text', function () { return this.textContent; }, function (v) { this.textContent = v; });
});
htmlClass('HTMLLinkElement', ['link'], (P) => {
	reflect(P, 'href', 'href', 'url'); reflect(P, 'rel'); reflect(P, 'type'); reflect(P, 'media'); reflect(P, 'as');
	def(P, 'relList', function () { return new DOMTokenList(this, 'rel'); });
	def(P, 'sheet', function () { return null; });
});
htmlClass('HTMLStyleElement', ['style'], (P) => { reflect(P, 'media'); reflect(P, 'type'); def(P, 'sheet', function () { return { cssRules: [], insertRule() { return 0; }, deleteRule() {} }; }); });
htmlClass('HTMLMetaElement', ['meta'], (P) => { reflect(P, 'name'); reflect(P, 'content'); reflect(P, 'httpEquiv', 'http-equiv'); });
htmlClass('HTMLIFrameElement', ['iframe'], (P) => {
	reflect(P, 'src', 'src', 'url'); reflect(P, 'name'); reflect(P, 'width'); reflect(P, 'height');
	def(P, 'contentWindow', function () { return null; });
	def(P, 'contentDocument', function () { return null; });
});
htmlClass('HTMLCanvasElement', ['canvas'], (P) => {
	def(P, 'width', function () { return parseInt(N.getAttr(this, 'width')) || 300; }, function (v) { N.setAttr(this, 'width', String(v)); });
	def(P, 'height', function () { return parseInt(N.getAttr(this, 'height')) || 150; }, function (v) { N.setAttr(this, 'height', String(v)); });
	P.getContext = function () { return null; };
	P.toDataURL = function () { return 'data:,'; };
});
htmlClass('HTMLTemplateElement', ['template'], (P) => {
	def(P, 'content', function () { const f = N.createFragment(); for (const c of childArray(this, false)) N.append(f, N.clone(c, true)); return f; });
});
htmlClass('HTMLLabelElement', ['label'], (P) => { reflect(P, 'htmlFor', 'for'); def(P, 'control', function () { return this.htmlFor ? G.document.getElementById(this.htmlFor) : this.querySelector('input,select,textarea'); }); });
htmlClass('HTMLTableElement', ['table'], (P) => {
	def(P, 'rows', function () { return makeList(qsa(this, 'tr', false)); });
	def(P, 'tBodies', function () { return makeList(childArray(this, true).filter((e) => lname(e) === 'tbody')); });
});
htmlClass('HTMLTableRowElement', ['tr'], (P) => { def(P, 'cells', function () { return makeList(childArray(this, true).filter((e) => /^t[dh]$/.test(lname(e)))); }); });
htmlClass('HTMLTableCellElement', ['td', 'th'], (P) => { reflect(P, 'colSpan', 'colspan', 'int'); reflect(P, 'rowSpan', 'rowspan', 'int'); });
htmlClass('HTMLDivElement', ['div']);
htmlClass('HTMLSpanElement', ['span']);
htmlClass('HTMLParagraphElement', ['p']);
htmlClass('HTMLHeadingElement', ['h1', 'h2', 'h3', 'h4', 'h5', 'h6']);
htmlClass('HTMLUListElement', ['ul']);
htmlClass('HTMLOListElement', ['ol']);
htmlClass('HTMLLIElement', ['li']);
htmlClass('HTMLBRElement', ['br']);
htmlClass('HTMLHRElement', ['hr']);
htmlClass('HTMLPreElement', ['pre']);
htmlClass('HTMLBodyElement', ['body']);
htmlClass('HTMLHeadElement', ['head']);
htmlClass('HTMLHtmlElement', ['html']);
htmlClass('HTMLTitleElement', ['title'], (P) => { def(P, 'text', function () { return this.textContent; }, function (v) { this.textContent = v; }); });
const media = (P) => {
	reflect(P, 'src', 'src', 'url');
	P.play = function () { return Promise.reject(new Error('NotSupportedError')); };
	P.pause = function () {}; P.load = function () {};
	P.canPlayType = function () { return ''; };
};
htmlClass('HTMLMediaElement', [], media);
htmlClass('HTMLVideoElement', ['video'], media);
htmlClass('HTMLAudioElement', ['audio'], media);
G.Audio = function Audio(src) { const a = G.document.createElement('audio'); if (src) a.src = src; return a; };
htmlClass('HTMLDetailsElement', ['details'], (P) => { reflect(P, 'open', 'open', 'bool'); });
htmlClass('HTMLDialogElement', ['dialog'], (P) => {
	reflect(P, 'open', 'open', 'bool');
	P.show = P.showModal = function () { this.open = true; };
	P.close = function (rv) { this.open = false; this.returnValue = rv; jsDispatch(this, new Event('close')); };
});
sub('HTMLUnknownElement', HTMLElement);
const SVGElement = sub('SVGElement', Element);
for (const t of ['svg', 'path', 'g', 'circle', 'rect', 'line', 'polyline', 'polygon', 'use', 'defs', 'symbol', 'ellipse'])
	ELPROTO[t] = SVGElement.prototype;
def(SVGElement.prototype, 'style', styleGetter);
def(SVGElement.prototype, 'dataset', datasetGetter);
def(SVGElement.prototype, 'className', function () { const c = N.getAttr(this, 'class') || ''; return { baseVal: c, animVal: c }; });

/* ---------------- Document ---------------- */
const Document = sub('Document', Node);
const HTMLDocument = sub('HTMLDocument', Document);
const DP = Document.prototype;
mixin(DP, PNM, PNG);
let docState = 'loading';
def(DP, 'documentElement', function () { for (let c = N.first(this); c; c = N.next(c)) if (N.nodeType(c) === 1) return c; return null; });
function rootChild(doc, tag) {
	const r = doc.documentElement;
	if (!r) return null;
	for (let c = N.first(r); c; c = N.next(c)) if (N.nodeType(c) === 1 && lname(c) === tag) return c;
	return null;
}
def(DP, 'head', function () { return rootChild(this, 'head'); });
def(DP, 'body', function () { return rootChild(this, 'body') || rootChild(this, 'frameset'); });
def(DP, 'title', function () {
	const t = qsa(this, 'title', true)[0];
	return t ? t.textContent.replace(/\s+/g, ' ').trim() : '';
}, function (v) {
	let t = qsa(this, 'title', true)[0];
	if (!t && this.head) { t = this.createElement('title'); this.head.appendChild(t); }
	if (t) t.textContent = v;
});
def(DP, 'URL', function () { return N.href(); });
def(DP, 'documentURI', function () { return N.href(); });
def(DP, 'baseURI', function () { return N.baseURL(); });
def(DP, 'location', function () { return G.location; }, function (v) { G.location.href = v; });
def(DP, 'domain', function () { return G.location.hostname; }, function () {});
def(DP, 'origin', function () { return G.location.origin; });
def(DP, 'cookie', function () { return N.cookie(); }, function (v) { N.cookie(String(v)); });
def(DP, 'readyState', function () { return docState; });
def(DP, 'referrer', function () { return ''; });
def(DP, 'characterSet', function () { return 'UTF-8'; });
def(DP, 'charset', function () { return 'UTF-8'; });
def(DP, 'inputEncoding', function () { return 'UTF-8'; });
def(DP, 'contentType', function () { return 'text/html'; });
def(DP, 'compatMode', function () { return 'CSS1Compat'; });
def(DP, 'defaultView', function () { return G; });
def(DP, 'visibilityState', function () { return 'visible'; });
def(DP, 'hidden', function () { return false; });
def(DP, 'activeElement', function () { return this.__active || this.body; });
def(DP, 'currentScript', function () { return null; });
def(DP, 'scrollingElement', function () { return this.documentElement; });
def(DP, 'doctype', function () { for (let c = N.first(this); c; c = N.next(c)) if (N.nodeType(c) === 10) return c; return null; });
def(DP, 'forms', function () { return makeList(qsa(this, 'form', false)); });
def(DP, 'images', function () { return makeList(qsa(this, 'img', false)); });
def(DP, 'links', function () { return makeList(qsa(this, 'a[href],area[href]', false)); });
def(DP, 'scripts', function () { return makeList(qsa(this, 'script', false)); });
def(DP, 'styleSheets', function () { return []; });
const fontSet = { ready: Promise.resolve(), status: 'loaded', check() { return true; }, load() { return Promise.resolve([]); }, add() {}, addEventListener() {}, forEach() {} };
def(DP, 'fonts', function () { return fontSet; });
/* createHTMLDocument: no second libdom document, so build a detached
 * html/head/body tree owned by the main document and wrap it (jQuery's
 * parseHTML feature test writes body.innerHTML; returning the live
 * document wiped the page, e.g. Wikipedia). */
function detachedDoc(title) {
	const D = G.document;
	const html = D.createElement('html'), head = D.createElement('head'), body = D.createElement('body');
	html.appendChild(head); html.appendChild(body);
	if (title !== undefined) { const t = D.createElement('title'); t.textContent = String(title); head.appendChild(t); }
	const doc = {
		nodeType: 9, nodeName: '#document', documentElement: html, head, body, defaultView: null,
		implementation: D.implementation, location: null, readyState: 'complete', contentType: 'text/html',
		createElement: (t) => D.createElement(t), createElementNS: (ns, t) => D.createElementNS ? D.createElementNS(ns, t) : D.createElement(t),
		createTextNode: (t) => D.createTextNode(t), createComment: (t) => D.createComment(t),
		createDocumentFragment: () => D.createDocumentFragment(), importNode: (n, d) => n.cloneNode(!!d), adoptNode: (n) => n,
		querySelector: (q) => html.querySelector(q), querySelectorAll: (q) => html.querySelectorAll(q),
		getElementById: (id) => html.querySelector('#' + cssEscape(id)),
		getElementsByTagName: (t) => html.getElementsByTagName(t), getElementsByClassName: (c) => html.getElementsByClassName(c),
		get title() { const t = head.querySelector('title'); return t ? t.textContent : ''; },
		addEventListener() {}, removeEventListener() {},
	};
	doc.ownerDocument = null;
	return doc;
}
def(DP, 'implementation', function () { return { hasFeature() { return true; }, createHTMLDocument(t) { return detachedDoc(t); },
	createDocument() { return detachedDoc(); }, createDocumentType(n) { return { nodeType: 10, name: n }; } }; });
DP.getElementById = function (id) { return N.byId(String(id)); };
DP.getElementsByName = function (n) { return makeList(qsa(this, '[name="' + String(n).replace(/"/g, '\\"') + '"]', false)); };
DP.createElement = function (tag) {
	tag = String(tag).toLowerCase();
	const d = ceCount ? CE.get(tag) : undefined;
	if (d && !d.ext) {
		try {
			const el = new d.ctor();
			if (!isNode(el) || N.nodeType(el) !== 1) throw new TypeError("custom element constructor for '" + tag + "' did not return an element");
			return el;
		} catch (e) { reportError(e); const u = N.createElement(tag); CES.set(u, false); return u; }
	}
	return N.createElement(tag);
};
DP.createElementNS = function (ns, tag) { return N.createElement(String(tag).replace(/^.*:/, '').toLowerCase()); };
DP.createTextNode = function (t) { return N.createText(String(t)); };
DP.createComment = function (t) { return N.createComment(String(t)); };
DP.createDocumentFragment = function () { return N.createFragment(); };
DP.createEvent = function (kind) {
	const k = String(kind).toLowerCase();
	const C = /keyboard/.test(k) ? KeyboardEvent : /mouse/.test(k) ? MouseEvent : /custom/.test(k) ? CustomEvent : Event;
	return new C('');
};
DP.createRange = function () {
	return { setStart() {}, setEnd() {}, selectNode() {}, selectNodeContents() {}, collapse() {}, getBoundingClientRect: zeroRect,
		createContextualFragment(html) { const f = N.createFragment(); parseHTMLInto(f, String(html)); return f; } };
};
/* live TreeWalker / NodeIterator (DOM spec traversal; filter: function or {acceptNode}) */
function twFilter(tw, n) {
	const w = tw.whatToShow >>> 0;
	if (!(w & (1 << (N.nodeType(n) - 1)))) return 3;
	const f = tw.filter;
	if (!f) return 1;
	const r = typeof f === 'function' ? f.call(null, n) : f.acceptNode(n);
	return Number(r) || 3;
}
class TreeWalker {
	constructor(root, what, filter) {
		val(this, 'root', root); val(this, 'whatToShow', what === undefined ? 0xFFFFFFFF : what >>> 0);
		val(this, 'filter', filter || null); this.currentNode = root;
	}
	parentNode() {
		let n = this.currentNode;
		while (n && n !== this.root) {
			n = N.parent(n);
			if (n && twFilter(this, n) === 1) { this.currentNode = n; return n; }
		}
		return null;
	}
	__child(first) {
		let n = first ? N.first(this.currentNode) : N.last(this.currentNode);
		while (n) {
			const r = twFilter(this, n);
			if (r === 1) { this.currentNode = n; return n; }
			if (r === 3) { const c = first ? N.first(n) : N.last(n); if (c) { n = c; continue; } }
			for (;;) {
				const s = first ? N.next(n) : N.prev(n);
				if (s) { n = s; break; }
				const p = N.parent(n);
				if (!p || p === this.root || p === this.currentNode) return null;
				n = p;
			}
		}
		return null;
	}
	firstChild() { return this.__child(true); }
	lastChild() { return this.__child(false); }
	__sib(next) {
		let n = this.currentNode;
		if (n === this.root) return null;
		for (;;) {
			let s = next ? N.next(n) : N.prev(n);
			while (s) {
				n = s;
				const r = twFilter(this, n);
				if (r === 1) { this.currentNode = n; return n; }
				s = next ? N.first(n) : N.last(n);
				if (r === 2 || !s) s = next ? N.next(n) : N.prev(n);
			}
			n = N.parent(n);
			if (!n || n === this.root) return null;
			if (twFilter(this, n) === 1) return null;
		}
	}
	nextSibling() { return this.__sib(true); }
	previousSibling() { return this.__sib(false); }
	previousNode() {
		let n = this.currentNode;
		while (n !== this.root) {
			let s = N.prev(n);
			while (s) {
				n = s;
				let r = twFilter(this, n);
				let c;
				while (r !== 2 && (c = N.last(n))) { n = c; r = twFilter(this, n); }
				if (r === 1) { this.currentNode = n; return n; }
				s = N.prev(n);
			}
			if (n === this.root) return null;
			const p = N.parent(n);
			if (!p) return null;
			n = p;
			if (twFilter(this, n) === 1) { this.currentNode = n; return n; }
		}
		return null;
	}
	nextNode() {
		let n = this.currentNode, r = 1;
		for (;;) {
			let c;
			while (r !== 2 && (c = N.first(n))) {
				n = c; r = twFilter(this, n);
				if (r === 1) { this.currentNode = n; return n; }
			}
			let s = null, t = n;
			while (t) {
				if (t === this.root) return null;
				s = N.next(t);
				if (s) break;
				t = N.parent(t);
			}
			if (!s) return null;
			n = s; r = twFilter(this, n);
			if (r === 1) { this.currentNode = n; return n; }
		}
	}
}
class NodeIterator {
	constructor(root, what, filter) {
		val(this, '__tw', new TreeWalker(root, what, filter));
		val(this, 'root', root); val(this, 'whatToShow', this.__tw.whatToShow); val(this, 'filter', filter || null);
		this.__before = true;
	}
	get referenceNode() { return this.__tw.currentNode; }
	get pointerBeforeReferenceNode() { return this.__before; }
	nextNode() {
		const tw = this.__tw;
		if (this.__before) { this.__before = false; if (twFilter(tw, tw.currentNode) === 1) return tw.currentNode; }
		/* NodeIterator: REJECT behaves like SKIP */
		let n = tw.currentNode;
		for (;;) {
			let x = N.first(n);
			if (!x) { let t = n; while (t && t !== tw.root && !N.next(t)) t = N.parent(t); if (!t || t === tw.root) return null; x = N.next(t); }
			n = x; tw.currentNode = n;
			if (twFilter(tw, n) === 1) return n;
		}
	}
	previousNode() {
		const tw = this.__tw;
		let n = tw.currentNode;
		if (!this.__before) { this.__before = true; if (twFilter(tw, n) === 1) return n; }
		for (;;) {
			if (n === tw.root) return null;
			let p = N.prev(n);
			if (p) { let c; while ((c = N.last(p))) p = c; n = p; } else n = N.parent(n);
			if (!n) return null;
			tw.currentNode = n;
			if (twFilter(tw, n) === 1) return n;
		}
	}
	detach() {}
}
G.TreeWalker = TreeWalker; G.NodeIterator = NodeIterator;
DP.createTreeWalker = function (root, what, filter) {
	if (!isNode(root)) throw new TypeError('createTreeWalker: root is not a Node');
	return new TreeWalker(root, what, filter);
};
DP.createNodeIterator = function (root, what, filter) {
	if (!isNode(root)) throw new TypeError('createNodeIterator: root is not a Node');
	return new NodeIterator(root, what, filter);
};
DP.write = function (...s) { N.docWrite(s.join('')); };
DP.writeln = function (...s) { N.docWrite(s.join('') + '\n'); };
DP.open = function () { return this; };
DP.close = function () {};
DP.hasFocus = function () { return true; };
DP.getSelection = function () { return G.getSelection(); };
DP.execCommand = function () { return false; };
DP.queryCommandSupported = function () { return false; };
DP.elementFromPoint = function () { return null; };
DP.adoptNode = function (n) { return n; };
DP.importNode = function (n, deep) { const c = N.clone(n, !!deep); if (ceCount) ceScan(c); return c; };
const DocumentType = sub('DocumentType', Node);
def(DocumentType.prototype, 'name', function () { return N.nodeName(this); });

/* ---------------- selectors ---------------- */
const selCache = new Map();
function cssEscape(s) { return String(s).replace(/([^a-zA-Z0-9_\u00A0-\uFFFF-])/g, '\\$1'); }
function parseSel(sel) {
	sel = String(sel).trim();
	const cached = selCache.get(sel);
	if (cached) return cached;
	const groups = [];
	let i = 0;
	const n = sel.length;
	const ws = () => { while (i < n && /\s/.test(sel[i])) i++; };
	const ident = () => {
		let s = '';
		while (i < n) {
			const ch = sel[i];
			if (ch === '\\') { s += sel[i + 1] || ''; i += 2; continue; }
			if (/[a-zA-Z0-9_\-\u00A0-\uFFFF]/.test(ch) || ch === '*') { s += ch; i++; } else break;
		}
		return s;
	};
	const compound = () => {
		const c = { tag: null, id: null, cls: [], attrs: [], pseudo: [] };
		let any = false;
		for (;;) {
			const ch = sel[i];
			if (ch === '#') { i++; c.id = ident(); any = true; }
			else if (ch === '.') { i++; c.cls.push(ident()); any = true; }
			else if (ch === '[') {
				i++; ws();
				const name = ident().toLowerCase(); ws();
				let op = null, v = null;
				if (sel[i] !== ']') {
					op = '';
					while (i < n && /[~|^$*=]/.test(sel[i])) op += sel[i++];
					ws();
					if (sel[i] === '"' || sel[i] === "'") {
						const q = sel[i++]; v = '';
						while (i < n && sel[i] !== q) { if (sel[i] === '\\') i++; v += sel[i++]; }
						i++;
					} else v = ident();
					ws();
					if (/^[iIsS]$/.test(sel[i] || '')) { i++; ws(); }
				}
				if (sel[i] === ']') i++;
				c.attrs.push({ name, op, v });
				any = true;
			} else if (ch === ':') {
				i++;
				if (sel[i] === ':') i++;
				const name = ident().toLowerCase();
				let arg = null;
				if (sel[i] === '(') {
					let depth = 1; i++; const st = i;
					while (i < n && depth) { if (sel[i] === '(') depth++; else if (sel[i] === ')') depth--; i++; }
					arg = sel.slice(st, i - 1).trim();
				}
				const subsel = (name === 'not' || name === 'is' || name === 'where' || name === 'matches') ? parseSel(arg) : null;
				c.pseudo.push({ name, arg, sub: subsel });
				any = true;
			} else if (ch && /[a-zA-Z*_\\-]/.test(ch) && !any) {
				c.tag = ident().toLowerCase(); if (c.tag === '*') c.tag = null; any = true;
			} else break;
		}
		return any ? c : null;
	};
	let cur = [];
	for (;;) {
		ws();
		const cmp = compound();
		if (!cmp) throw new SyntaxError("'" + sel + "' is not a valid selector");
		cur.push(cmp);
		const before = i; ws();
		if (i >= n) break;
		const ch = sel[i];
		if (ch === ',') { i++; groups.push(cur); cur = []; continue; }
		if (ch === '>' || ch === '+' || ch === '~') { i++; cur.push(ch); continue; }
		if (i > before) { cur.push(' '); continue; }
		throw new SyntaxError("'" + sel + "' is not a valid selector");
	}
	groups.push(cur);
	if (selCache.size > 500) selCache.clear();
	selCache.set(sel, groups);
	return groups;
}
function nthMatch(arg, idx) {
	arg = String(arg).replace(/\s+/g, '').toLowerCase();
	if (arg === 'odd') return idx % 2 === 1;
	if (arg === 'even') return idx % 2 === 0;
	const m = /^([+-]?\d*)n([+-]\d+)?$/.exec(arg);
	if (!m) return idx === parseInt(arg);
	const a = m[1] === '' || m[1] === '+' ? 1 : m[1] === '-' ? -1 : parseInt(m[1]);
	const b = m[2] ? parseInt(m[2]) : 0;
	if (a === 0) return idx === b;
	return (idx - b) / a >= 0 && (idx - b) % a === 0;
}
function elIndex(el, fromEnd, sameType) {
	let i = 1;
	const t = sameType ? lname(el) : null;
	for (let s = fromEnd ? N.next(el) : N.prev(el); s; s = fromEnd ? N.next(s) : N.prev(s))
		if (N.nodeType(s) === 1 && (!t || lname(s) === t)) i++;
	return i;
}
function matchCompound(el, c) {
	if (N.nodeType(el) !== 1) return false;
	if (c.tag && lname(el) !== c.tag) return false;
	if (c.id !== null && N.getAttr(el, 'id') !== c.id) return false;
	if (c.cls.length) { const cl = classesOf(el); for (const x of c.cls) if (!cl.includes(x)) return false; }
	for (const a of c.attrs) {
		const v = N.getAttr(el, a.name);
		if (v === null || v === undefined) return false;
		if (a.op === null) continue;
		switch (a.op) {
		case '=': if (v !== a.v) return false; break;
		case '~=': if (!v.split(/\s+/).includes(a.v)) return false; break;
		case '|=': if (v !== a.v && !v.startsWith(a.v + '-')) return false; break;
		case '^=': if (!a.v || !v.startsWith(a.v)) return false; break;
		case '$=': if (!a.v || !v.endsWith(a.v)) return false; break;
		case '*=': if (!a.v || v.indexOf(a.v) < 0) return false; break;
		default: return false;
		}
	}
	for (const p of c.pseudo) {
		switch (p.name) {
		case 'not': if (matchesSel(el, p.sub)) return false; break;
		case 'is': case 'where': case 'matches': if (!matchesSel(el, p.sub)) return false; break;
		case 'has': if (!qsa(el, p.arg, true).length) return false; break;
		case 'first-child': if (elIndex(el, false) !== 1) return false; break;
		case 'last-child': if (elIndex(el, true) !== 1) return false; break;
		case 'only-child': if (elIndex(el, false) !== 1 || elIndex(el, true) !== 1) return false; break;
		case 'first-of-type': if (elIndex(el, false, true) !== 1) return false; break;
		case 'last-of-type': if (elIndex(el, true, true) !== 1) return false; break;
		case 'nth-child': if (!nthMatch(p.arg, elIndex(el, false))) return false; break;
		case 'nth-last-child': if (!nthMatch(p.arg, elIndex(el, true))) return false; break;
		case 'nth-of-type': if (!nthMatch(p.arg, elIndex(el, false, true))) return false; break;
		case 'checked': if (!(N.checked(el) || N.hasAttr(el, 'selected'))) return false; break;
		case 'disabled': if (!N.hasAttr(el, 'disabled')) return false; break;
		case 'enabled': if (N.hasAttr(el, 'disabled')) return false; break;
		case 'empty': if (N.first(el)) return false; break;
		case 'root': { const p2 = N.parent(el); if (!p2 || N.nodeType(p2) !== 9) return false; break; }
		case 'link': case 'any-link': if (!(lname(el) === 'a' && N.hasAttr(el, 'href'))) return false; break;
		case 'scope': break;
		case 'hover': case 'active': case 'focus': case 'focus-within': case 'focus-visible': case 'visited': case 'target': return false;
		default: break; /* unknown pseudo (e.g. ::before): ignore */
		}
	}
	return true;
}
function matchChain(el, chain, idx) {
	if (!matchCompound(el, chain[idx])) return false;
	if (idx === 0) return true;
	const comb = chain[idx - 1];
	const prevIdx = idx - 2;
	if (comb === '>') { const p = N.parent(el); return !!p && matchChain(p, chain, prevIdx); }
	if (comb === ' ') { for (let p = N.parent(el); p; p = N.parent(p)) if (matchChain(p, chain, prevIdx)) return true; return false; }
	if (comb === '+') { let s = N.prev(el); while (s && N.nodeType(s) !== 1) s = N.prev(s); return !!s && matchChain(s, chain, prevIdx); }
	if (comb === '~') { for (let s = N.prev(el); s; s = N.prev(s)) if (N.nodeType(s) === 1 && matchChain(s, chain, prevIdx)) return true; return false; }
	return false;
}
function matchesSel(el, groups) {
	for (const g of groups) if (matchChain(el, g, g.length - 1)) return true;
	return false;
}
function qsa(root, sel, first) {
	const groups = parseSel(sel);
	/* fast path: #id */
	if (groups.length === 1 && groups[0].length === 1) {
		const c = groups[0][0];
		if (c.id && !c.tag && !c.cls.length && !c.attrs.length && !c.pseudo.length) {
			const d = N.nodeType(root) === 9 ? root : N.ownerDoc(root);
			const e = d ? N.byId(c.id) : null;
			if (e && e !== root && (root === d || root.contains(e))) return [e];
			if (root === d) return [];
		}
	}
	/* Candidates come from a native walk filtered by each group's rightmost
	 * compound (tag / id / classes); a bare compound needs no JS match, and a
	 * chain that starts with #id only searches under that element. */
	const filtered = (c) => c.tag !== null || c.id !== null || c.cls.length > 0;
	if (groups.length === 1 || groups.every((g) => filtered(g[g.length - 1]))) {
		const res = [];
		for (const g of groups) res.push(groupMatches(root, g, first && groups.length === 1));
		if (res.length === 1) return res[0];
		const seen = new Set();
		for (const r of res) for (const n of r) seen.add(n);
		if (!seen.size) return [];
		let out;
		if (seen.size > 64) out = N.collect(root, null, null, null, false).filter((n) => seen.has(n));
		else out = [...seen].sort(docOrderCmp);
		return first ? out.slice(0, 1) : out;
	}
	const out = [];
	for (const n of N.collect(root, null, null, null, false))
		if (matchesSel(n, groups)) { out.push(n); if (first) break; }
	return out;
}
function docOrderCmp(a, b) {
	return a === b ? 0 : (NP.compareDocumentPosition.call(a, b) & 4) ? -1 : 1;
}
function groupMatches(root, g, first) {
	const c = g[g.length - 1];
	const simple = g.length === 1 && !c.attrs.length && !c.pseudo.length;
	const cls = c.cls.length ? c.cls : null;
	if (simple) return N.collect(root, c.tag, c.id, cls, first);
	let scopes = [root];
	const a = g[0];
	if (g.length > 2 && (a.tag !== null || a.id !== null || a.cls.length) && (g[1] === ' ' || g[1] === '>')) {
		/* A ... X: X must lie inside an element matching A, so only the
		 * subtrees of such anchors are searched. */
		let top = root;
		for (let p = N.parent(top); p; p = N.parent(p)) top = p;
		const anchors = N.collect(top, a.tag, a.id, a.cls.length ? a.cls : null, false);
		if (anchors.length <= 200) {
			scopes = [];
			for (const e of anchors) {
				if (!matchCompound(e, a)) continue;
				if (e === root || NP.contains.call(e, root)) { scopes = [root]; break; }
				if (NP.contains.call(root, e)) scopes.push(e);
			}
		}
	}
	if (scopes.length > 1) {
		/* anchors are in document order: drop ones nested in the last kept */
		const keep = [];
		for (const e of scopes) if (!keep.length || !NP.contains.call(keep[keep.length - 1], e)) keep.push(e);
		scopes = keep;
	}
	const out = [];
	for (const sc of scopes) {
		for (const n of N.collect(sc, c.tag, c.id, cls, false))
			if (matchChain(n, g, g.length - 1)) { out.push(n); if (first) return out; }
	}
	return out;
}

/* ---------------- HTML fragment parser / serializer ---------------- */
const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr']);
const RAW = new Set(['script', 'style', 'textarea', 'title', 'xmp']);
const ENT = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: '\u00a0', copy: '\u00a9', reg: '\u00ae',
	times: '\u00d7', divide: '\u00f7', hellip: '\u2026', mdash: '\u2014', ndash: '\u2013', laquo: '\u00ab',
	raquo: '\u00bb', middot: '\u00b7', bull: '\u2022', rarr: '\u2192', larr: '\u2190', uarr: '\u2191',
	darr: '\u2193', check: '\u2713', euro: '\u20ac', pound: '\u00a3', deg: '\u00b0', trade: '\u2122',
	lsquo: '\u2018', rsquo: '\u2019', ldquo: '\u201c', rdquo: '\u201d', shy: '\u00ad', para: '\u00b6', sect: '\u00a7' };
function decodeEnt(s) {
	if (s.indexOf('&') < 0) return s;
	return s.replace(/&(#[xX][0-9a-fA-F]+|#\d+|[a-zA-Z][a-zA-Z0-9]*);?/g, (m, e) => {
		if (e[0] === '#') {
			const cp = e[1] === 'x' || e[1] === 'X' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
			try { return String.fromCodePoint(cp); } catch (x) { return m; }
		}
		return ENT[e] !== undefined ? ENT[e] : m;
	});
}
const IMPLIED_END = { p: /^(address|article|aside|blockquote|div|dl|fieldset|footer|form|h[1-6]|header|hr|main|nav|ol|p|pre|section|table|ul)$/,
	li: /^li$/, option: /^option$/, dt: /^(dt|dd)$/, dd: /^(dt|dd)$/, tr: /^tr$/, td: /^(td|th|tr)$/, th: /^(td|th|tr)$/ };
function parseHTMLInto(parent, html) {
	const stack = [parent];
	const top = () => stack[stack.length - 1];
	let i = 0;
	const n = html.length;
	const text = (s) => { if (s) N.append(top(), N.createText(decodeEnt(s))); };
	while (i < n) {
		const lt = html.indexOf('<', i);
		if (lt < 0) { text(html.slice(i)); break; }
		if (lt > i) text(html.slice(i, lt));
		i = lt;
		if (html.startsWith('<!--', i)) {
			const e = html.indexOf('-->', i + 4);
			N.append(top(), N.createComment(html.slice(i + 4, e < 0 ? n : e)));
			i = e < 0 ? n : e + 3;
			continue;
		}
		if (html[i + 1] === '!' || html[i + 1] === '?') { const e = html.indexOf('>', i); i = e < 0 ? n : e + 1; continue; }
		if (html[i + 1] === '/') {
			const m = /^<\/\s*([a-zA-Z][^\s>\/]*)[^>]*>/.exec(html.slice(i, i + 128));
			if (!m) { text('<'); i++; continue; }
			const tag = m[1].toLowerCase();
			for (let k = stack.length - 1; k > 0; k--) {
				if (lname(stack[k]) === tag) { stack.length = k; break; }
			}
			i += m[0].length;
			continue;
		}
		const m = /^<([a-zA-Z][^\s\/>]*)/.exec(html.slice(i, i + 64));
		if (!m) { text('<'); i++; continue; }
		const tag = m[1].toLowerCase();
		i += m[0].length;
		const attrs = [];
		let selfClose = false;
		for (;;) {
			while (i < n && /\s/.test(html[i])) i++;
			if (i >= n) break;
			if (html[i] === '>') { i++; break; }
			if (html[i] === '/' && html[i + 1] === '>') { selfClose = true; i += 2; break; }
			if (html[i] === '/') { i++; continue; }
			const am = /^[^\s=\/>]+/.exec(html.slice(i, i + 256));
			if (!am) { i++; continue; }
			const an = am[0].toLowerCase();
			i += am[0].length;
			while (i < n && /\s/.test(html[i])) i++;
			let av = '';
			if (html[i] === '=') {
				i++;
				while (i < n && /\s/.test(html[i])) i++;
				if (html[i] === '"' || html[i] === "'") {
					const q = html[i];
					const e = html.indexOf(q, i + 1);
					av = html.slice(i + 1, e < 0 ? n : e);
					i = e < 0 ? n : e + 1;
				} else {
					const vm = /^[^\s>]*/.exec(html.slice(i, i + 4096));
					av = vm[0];
					i += av.length;
				}
			}
			attrs.push([an, decodeEnt(av)]);
		}
		const cur = top();
		if (cur !== parent && N.nodeType(cur) === 1) {
			const re = IMPLIED_END[lname(cur)];
			if (re && re.test(tag)) stack.pop();
		}
		let el;
		try { el = N.createElement(tag); } catch (e) { continue; }
		for (const [an, av] of attrs) { try { N.setAttr(el, an, av); } catch (e) { /* invalid attribute name */ } }
		N.append(top(), el);
		for (const [an] of attrs) if (an.startsWith('on') && an.length > 2) N.listen(el, an.slice(2));
		if (RAW.has(tag)) {
			const rest = html.slice(i);
			const em = new RegExp('</' + tag + '\\s*>', 'i').exec(rest);
			const body = em ? rest.slice(0, em.index) : rest;
			if (body) N.append(el, N.createText(tag === 'textarea' || tag === 'title' ? decodeEnt(body) : body));
			i += em ? em.index + em[0].length : rest.length;
			continue;
		}
		if (!VOID.has(tag) && !selfClose) stack.push(el);
	}
}
const escText = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/\u00a0/g, '&nbsp;');
const escAttr = (s) => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/\u00a0/g, '&nbsp;');
function serialize(n) {
	const t = N.nodeType(n);
	if (t === 3) {
		const p = N.parent(n);
		if (p && N.nodeType(p) === 1) { const pn = lname(p); if (pn === 'script' || pn === 'style' || pn === 'xmp') return N.nodeValue(n) || ''; }
		return escText(N.nodeValue(n) || '');
	}
	if (t === 8) return '<!--' + (N.nodeValue(n) || '') + '-->';
	if (t === 1) {
		const tag = lname(n);
		let s = '<' + tag;
		for (const [k, v] of N.attrs(n)) s += ' ' + k + '="' + escAttr(v) + '"';
		s += '>';
		if (VOID.has(tag)) return s;
		return s + serializeChildren(n) + '</' + tag + '>';
	}
	if (t === 9 || t === 11) return serializeChildren(n);
	return '';
}
function serializeChildren(n) {
	let s = '';
	for (let c = N.first(n); c; c = N.next(c)) s += serialize(c);
	return s;
}

class DOMParser {
	parseFromString(str) {
		/* no second document: parse into a detached fragment */
		const f = N.createFragment();
		parseHTMLInto(f, String(str));
		const one = (s) => qsa(f, s, true)[0] || null;
		return { body: one('body') || f, documentElement: one('html') || f, head: one('head'),
			querySelector: (s) => one(s), querySelectorAll: (s) => makeList(qsa(f, s, false)),
			getElementById: (id) => one('#' + cssEscape(id)),
			getElementsByTagName: (t) => PNM.getElementsByTagName.call(f, t), get textContent() { return f.textContent; } };
	}
}
G.DOMParser = DOMParser;
G.XMLSerializer = class XMLSerializer { serializeToString(n) { return serialize(n); } };

/* ---------------- URL / URLSearchParams ---------------- */
const qdec = (x) => { try { return decodeURIComponent(x.replace(/\+/g, ' ')); } catch (e) { return x; } };
class URLSearchParams {
	constructor(init) {
		val(this, '__l', []);
		val(this, '__url', null);
		if (init === undefined || init === null) return;
		if (init instanceof URLSearchParams || init instanceof FormData) { for (const [k, v] of init.entries()) this.__l.push([k, String(v)]); return; }
		if (typeof init === 'object') {
			if (typeof init[Symbol.iterator] === 'function') { for (const [k, v] of init) this.__l.push([String(k), String(v)]); }
			else for (const k of Object.keys(init)) this.__l.push([k, String(init[k])]);
			return;
		}
		let s = String(init);
		if (s[0] === '?') s = s.slice(1);
		for (const part of s.split('&')) {
			if (!part) continue;
			const i = part.indexOf('=');
			this.__l.push(i < 0 ? [qdec(part), ''] : [qdec(part.slice(0, i)), qdec(part.slice(i + 1))]);
		}
	}
	append(k, v) { this.__l.push([String(k), String(v)]); this.__upd(); }
	delete(k) { k = String(k); for (let i = this.__l.length - 1; i >= 0; i--) if (this.__l[i][0] === k) this.__l.splice(i, 1); this.__upd(); }
	get(k) { const p = this.__l.find((x) => x[0] === String(k)); return p ? p[1] : null; }
	getAll(k) { return this.__l.filter((x) => x[0] === String(k)).map((x) => x[1]); }
	has(k) { return this.__l.some((x) => x[0] === String(k)); }
	set(k, v) {
		k = String(k);
		const i = this.__l.findIndex((x) => x[0] === k);
		if (i < 0) this.__l.push([k, String(v)]);
		else { this.__l[i][1] = String(v); for (let j = this.__l.length - 1; j > i; j--) if (this.__l[j][0] === k) this.__l.splice(j, 1); }
		this.__upd();
	}
	sort() { this.__l.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); this.__upd(); }
	forEach(f, t) { for (const [k, v] of this.__l) f.call(t, v, k, this); }
	keys() { return this.__l.map((x) => x[0])[Symbol.iterator](); }
	values() { return this.__l.map((x) => x[1])[Symbol.iterator](); }
	entries() { return this.__l.map((x) => [x[0], x[1]])[Symbol.iterator](); }
	[Symbol.iterator]() { return this.entries(); }
	get size() { return this.__l.length; }
	toString() {
		const enc = (s) => encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, (c) => '%' + c.charCodeAt(0).toString(16).toUpperCase());
		return this.__l.map(([k, v]) => enc(k) + '=' + enc(v)).join('&');
	}
	__upd() { if (this.__url) { const s = this.toString(); this.__url.__search = s ? '?' + s : ''; } }
}
G.URLSearchParams = URLSearchParams;

const URL_RE = /^([a-zA-Z][a-zA-Z0-9+.\-]*:)(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
const DEFPORT = { 'http:': '80', 'https:': '443', 'ftp:': '21', 'ws:': '80', 'wss:': '443' };
class URL {
	constructor(url, base) {
		url = String(url).trim();
		let abs;
		if (base !== undefined && base !== null) abs = resolveURL(url, base instanceof URL ? base : new URL(String(base)));
		else if (/^[a-zA-Z][a-zA-Z0-9+.\-]*:/.test(url)) abs = url;
		else throw new TypeError("Invalid URL: '" + url + "'");
		this.__parse(abs);
	}
	__parse(abs) {
		const m = URL_RE.exec(abs);
		if (!m) throw new TypeError("Invalid URL: '" + abs + "'");
		val(this, '__protocol', m[1].toLowerCase());
		val(this, '__username', m[2] || '');
		val(this, '__password', m[3] || '');
		val(this, '__hostname', (m[4] || '').toLowerCase());
		let port = m[5] || '';
		if (DEFPORT[this.__protocol] === port) port = '';
		val(this, '__port', port);
		val(this, '__hier', abs.slice(m[1].length, m[1].length + 2) === '//');
		let path = m[6] || '';
		if (this.__hier && !path) path = '/';
		val(this, '__pathname', normPath(path, this.__hier));
		val(this, '__search', m[7] && m[7] !== '?' ? m[7] : '');
		val(this, '__hash', m[8] && m[8] !== '#' ? m[8] : '');
		val(this, '__sp', null);
	}
	get href() {
		const auth = this.__username ? this.__username + (this.__password ? ':' + this.__password : '') + '@' : '';
		const hier = this.__hier ? '//' + auth + this.host : '';
		return this.__protocol + hier + this.__pathname + this.__search + this.__hash;
	}
	set href(v) { this.__parse(String(v)); }
	get protocol() { return this.__protocol; }
	set protocol(v) { v = String(v); this.__protocol = (v.endsWith(':') ? v : v + ':').toLowerCase(); }
	get host() { return this.__hostname + (this.__port ? ':' + this.__port : ''); }
	set host(v) { const p = String(v).split(':'); this.__hostname = p[0].toLowerCase(); this.__port = p[1] || ''; }
	get hostname() { return this.__hostname; }
	set hostname(v) { this.__hostname = String(v).toLowerCase(); }
	get port() { return this.__port; }
	set port(v) { this.__port = String(v); }
	get pathname() { return this.__pathname; }
	set pathname(v) { v = String(v); this.__pathname = v[0] === '/' ? v : '/' + v; }
	get search() { return this.__search; }
	set search(v) {
		v = String(v);
		this.__search = v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : '';
		if (this.__sp) this.__sp.__l.splice(0, this.__sp.__l.length, ...new URLSearchParams(this.__search).__l);
	}
	get hash() { return this.__hash; }
	set hash(v) { v = String(v); this.__hash = v && v !== '#' ? (v[0] === '#' ? v : '#' + v) : ''; }
	get username() { return this.__username; }
	get password() { return this.__password; }
	get origin() { return this.__hostname ? this.__protocol + '//' + this.host : 'null'; }
	get searchParams() {
		if (!this.__sp) { this.__sp = new URLSearchParams(this.__search); this.__sp.__url = this; }
		return this.__sp;
	}
	toString() { return this.href; }
	toJSON() { return this.href; }
	static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
	static createObjectURL() { return 'blob:null/0'; }
	static revokeObjectURL() {}
}
function normPath(p, hier) {
	if (!hier || p.indexOf('.') < 0) return p;
	const out = [];
	const segs = p.split('/');
	for (let i = 0; i < segs.length; i++) {
		const s = segs[i];
		const last = i === segs.length - 1;
		if (s === '..') { if (out.length > 1) out.pop(); if (last) out.push(''); }
		else if (s === '.') { if (last) out.push(''); }
		else out.push(s);
	}
	const r = out.join('/');
	return r[0] === '/' ? r : '/' + r;
}
function resolveURL(rel, b) {
	if (/^[a-zA-Z][a-zA-Z0-9+.\-]*:/.test(rel)) return rel;
	const auth = b.__username ? b.__username + (b.__password ? ':' + b.__password : '') + '@' : '';
	const origin = b.__protocol + '//' + auth + b.host;
	if (rel.startsWith('//')) return b.__protocol + rel;
	if (rel === '') return origin + b.__pathname + b.__search;
	if (rel[0] === '#') return origin + b.__pathname + b.__search + rel;
	if (rel[0] === '?') return origin + b.__pathname + rel;
	if (rel[0] === '/') return origin + rel;
	const dir = b.__pathname.replace(/[^\/]*$/, '');
	return origin + (dir || '/') + rel;
}
G.URL = URL;
G.webkitURL = URL;
function urlParts(P, getHref, setHref) {
	for (const k of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin']) {
		def(P, k, function () { try { return new URL(getHref.call(this))[k]; } catch (e) { return ''; } },
			k === 'origin' ? undefined : function (v) { const u = new URL(getHref.call(this)); u[k] = v; setHref.call(this, u.href); });
	}
}

/* ---------------- window ---------------- */
winTarget = G;
for (const k of Object.keys(ETP)) val(G, k, ETP[k]);
defineOn(G);
G.window = G; G.self = G; G.top = G; G.parent = G; G.frames = G;
G.frameElement = null; G.opener = null; G.closed = false; G.name = '';
G.isSecureContext = /^https:/.test(N.href());
G.origin = (() => { try { return new URL(N.href()).origin; } catch (e) { return 'null'; } })();
let docCache = null;
def(G, 'document', function () { if (!docCache) docCache = N.document(); return docCache; });
G.Window = function Window() { illegal(); };
G.EventTarget = EventTarget; G.Node = Node; G.Element = Element; G.HTMLElement = HTMLElement;
G.Event = Event; G.UIEvent = UIEvent;
G.CustomEvent = CustomEvent; G.KeyboardEvent = KeyboardEvent; G.MouseEvent = MouseEvent; G.PointerEvent = PointerEvent;
G.FocusEvent = FocusEvent; G.InputEvent = InputEvent; G.ErrorEvent = ErrorEvent; G.ProgressEvent = ProgressEvent;
G.MessageEvent = MessageEvent; G.StorageEvent = StorageEvent; G.PopStateEvent = PopStateEvent;
G.HashChangeEvent = HashChangeEvent; G.TouchEvent = TouchEvent;
G.NodeList = Array; G.HTMLCollection = Array;
G.NodeFilter = { SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_TEXT: 4, SHOW_COMMENT: 128, FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3 };

/* location */
class Location {
	get href() { return N.href(); }
	set href(v) { N.navigate(String(v)); }
	assign(v) { N.navigate(String(v)); }
	replace(v) { N.navigate(String(v)); }
	reload() { N.navigate(null); }
	toString() { return N.href(); }
	get ancestorOrigins() { return []; }
}
urlParts(Location.prototype, function () { return N.href(); }, function (v) { N.navigate(v); });
const location = new Location();
def(G, 'location', function () { return location; }, function (v) { N.navigate(String(v)); });
G.Location = Location;

/* history */
const histStack = [];
G.history = {
	get length() { return 1 + histStack.length; },
	get state() { return histStack.length ? histStack[histStack.length - 1] : null; },
	scrollRestoration: 'auto',
	pushState(s) { histStack.push(s); },
	replaceState(s) { if (histStack.length) histStack[histStack.length - 1] = s; },
	back() {}, forward() {}, go() {},
};

/* navigator / screen */
const ua = N.ua();
val(G, "navigator", {
	userAgent: ua, appVersion: ua.replace(/^[^\/]*\//, ''), appName: 'Netscape', appCodeName: 'Mozilla',
	product: 'Gecko', productSub: '20030107', vendor: '', vendorSub: '', platform: 'Linux x86_64',
	language: 'en-US', languages: ['en-US', 'en'], cookieEnabled: true, onLine: true, doNotTrack: null,
	hardwareConcurrency: 1, maxTouchPoints: 0, webdriver: false, pdfViewerEnabled: false,
	javaEnabled() { return false; }, sendBeacon() { return false; },
	plugins: [], mimeTypes: [],
});
G.Navigator = function Navigator() { illegal(); };
G.screen = { get width() { return N.dims()[0]; }, get height() { return N.dims()[1]; },
	get availWidth() { return N.dims()[0]; }, get availHeight() { return N.dims()[1]; },
	colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0, addEventListener() {} } };
def(G, 'innerWidth', function () { return N.dims()[0]; });
def(G, 'innerHeight', function () { return N.dims()[1]; });
def(G, 'outerWidth', function () { return N.dims()[0]; });
def(G, 'outerHeight', function () { return N.dims()[1]; });
G.devicePixelRatio = 1;
G.scrollX = G.scrollY = G.pageXOffset = G.pageYOffset = 0;
G.screenX = G.screenY = G.screenLeft = G.screenTop = 0;
G.scrollTo = G.scrollBy = G.scroll = G.focus = G.blur = G.moveTo = G.resizeTo = G.print = G.stop = function () {};
G.close = function () {};
G.open = function (url) { if (url) N.navigate(String(url)); return null; };
G.alert = function (m) { N.log(3, 'alert: ' + (m === undefined ? '' : String(m))); };
G.confirm = function (m) { N.log(3, 'confirm: ' + String(m) + ' -> true'); return true; };
G.prompt = function (m, d) { N.log(3, 'prompt: ' + String(m)); return d === undefined ? null : String(d); };
G.postMessage = function (data) { const ev = new MessageEvent('message', { data }); setTimeout(() => hookWindow('message', ev), 0); };
G.getSelection = function () { return { rangeCount: 0, toString() { return ''; }, removeAllRanges() {}, addRange() {}, getRangeAt() { return null; } }; };
G.getComputedStyle = function (el) {
	const inl = el && el.nodeType === 1 ? styleFor(el, true) : null;
	return new Proxy({}, {
		get(t, p) {
			if (typeof p !== 'string') return undefined;
			if (p === 'getPropertyValue') return (k) => (inl ? inl.getPropertyValue(k) : '') || dfltStyle(el, String(k));
			const v = inl ? inl[p] : '';
			if (v === '' || v === undefined) return dfltStyle(el, camelToKebab(p));
			return v;
		},
	});
};
function dfltStyle(el, k) {
	if (k === 'display') {
		if (el && el.hasAttribute && el.hasAttribute('hidden')) return 'none';
		const t = el && el.localName;
		return /^(span|a|b|i|em|strong|img|input|button|select|label|code|small|sup|sub)$/.test(t) ? 'inline' : 'block';
	}
	if (k === 'visibility') return 'visible';
	if (k === 'opacity') return '1';
	if (k === 'position') return 'static';
	return '';
}
G.matchMedia = function (q) {
	q = String(q);
	const d = N.dims();
	let matches = false;
	const mw = /max-width:\s*(\d+)px/.exec(q), nw = /min-width:\s*(\d+)px/.exec(q);
	if (mw || nw) matches = (!mw || d[0] <= +mw[1]) && (!nw || d[0] >= +nw[1]);
	else if (/prefers-color-scheme:\s*light/.test(q) || /^\s*(screen|all)\s*$/.test(q)) matches = true;
	return { matches, media: q, onchange: null, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {}, dispatchEvent() { return true; } };
};

/* timers */
let timerSeq = 0;
const TIM = new Map();
function addTimer(fn, ms, args, repeat) {
	const id = ++timerSeq;
	ms = Number(ms) || 0;
	TIM.set(id, { fn, args, repeat });
	if (!N.timer(id, ms, repeat)) TIM.delete(id);
	return id;
}
G.setTimeout = function (fn, ms, ...args) { return addTimer(fn, ms, args, false); };
G.setInterval = function (fn, ms, ...args) { return addTimer(fn, ms, args, true); };
G.clearTimeout = G.clearInterval = function (id) { id = Number(id); if (TIM.has(id)) { TIM.delete(id); N.cancel(id); } };
function hookTimer(id) {
	const t = TIM.get(id);
	if (!t) return;
	if (!t.repeat) TIM.delete(id);
	try {
		if (typeof t.fn === 'function') t.fn.apply(G, t.args);
		else (0, eval)(String(t.fn));
	} catch (e) { reportError(e); }
}
G.requestAnimationFrame = function (fn) { return addTimer(() => fn(G.performance.now()), 50, [], false); };
G.cancelAnimationFrame = G.clearTimeout;
G.requestIdleCallback = function (fn) { return addTimer(() => fn({ didTimeout: false, timeRemaining() { return 10; } }), 1, [], false); };
G.cancelIdleCallback = G.clearTimeout;
G.queueMicrotask = function (fn) { Promise.resolve().then(fn).catch(reportError); };
G.setImmediate = function (fn, ...a) { return addTimer(fn, 0, a, false); };

const t0 = N.now();
val(G, "performance", { now() { return N.now() - t0; }, timeOrigin: Date.now(), mark() {}, measure() {},
	getEntriesByType() { return []; }, getEntriesByName() { return []; }, getEntries() { return []; },
	clearMarks() {}, clearMeasures() {}, timing: { navigationStart: Date.now() }, navigation: { type: 0 } });

/* storage */
function makeStorage(persist) {
	let data = null;
	const load = () => {
		if (data) return data;
		data = Object.create(null);
		if (persist) {
			const s = N.storage();
			if (s) { try { const o = JSON.parse(s); for (const k of Object.keys(o)) data[k] = String(o[k]); } catch (e) { /* corrupt file */ } }
		}
		return data;
	};
	const save = () => { if (persist) N.storage(JSON.stringify(data)); };
	const api = {
		getItem(k) { const d = load(); k = String(k); return k in d ? d[k] : null; },
		setItem(k, v) { load()[String(k)] = String(v); save(); },
		removeItem(k) { delete load()[String(k)]; save(); },
		clear() { data = Object.create(null); save(); },
		key(i) { return Object.keys(load())[i] || null; },
		get length() { return Object.keys(load()).length; },
	};
	return new Proxy(api, {
		get(t, p) { if (typeof p === 'symbol' || p in api) return Reflect.get(t, p); const d = load(); return p in d ? d[p] : undefined; },
		set(t, p, v) { if (p in api) return true; api.setItem(p, v); return true; },
		deleteProperty(t, p) { api.removeItem(p); return true; },
		has(t, p) { return p in api || p in load(); },
		ownKeys() { return Object.keys(load()); },
		getOwnPropertyDescriptor(t, p) { const d = load(); return p in d ? { value: d[p], enumerable: true, configurable: true, writable: true } : undefined; },
	});
}
const localStorage = makeStorage(true);
const sessionStorage = makeStorage(false);
def(G, 'localStorage', function () { return localStorage; });
def(G, 'sessionStorage', function () { return sessionStorage; });
G.Storage = function Storage() { illegal(); };

/* base64, text codecs */
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
G.btoa = function (s) {
	s = String(s);
	let out = '';
	for (let i = 0; i < s.length; i += 3) {
		const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
		if (a > 255 || b > 255 || c > 255) throw new Error('InvalidCharacterError');
		const n = (a << 16) | ((b || 0) << 8) | (c || 0);
		out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + (i + 1 < s.length ? B64[(n >> 6) & 63] : '=') + (i + 2 < s.length ? B64[n & 63] : '=');
	}
	return out;
};
G.atob = function (s) {
	s = String(s).replace(/[\s=]/g, '');
	let out = '', bits = 0, acc = 0;
	for (const ch of s) {
		const v = B64.indexOf(ch);
		if (v < 0) throw new Error('InvalidCharacterError');
		acc = ((acc << 6) | v) & 0xffffff; bits += 6;
		if (bits >= 8) { bits -= 8; out += String.fromCharCode((acc >> bits) & 255); }
	}
	return out;
};
class TextEncoder {
	get encoding() { return 'utf-8'; }
	encode(s) {
		s = unescape(encodeURIComponent(String(s === undefined ? '' : s)));
		const a = new Uint8Array(s.length);
		for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i);
		return a;
	}
}
class TextDecoder {
	constructor(label) { this.encoding = (label || 'utf-8').toLowerCase(); }
	decode(buf) {
		if (!buf) return '';
		const a = buf instanceof ArrayBuffer ? new Uint8Array(buf) : new Uint8Array(buf.buffer, buf.byteOffset || 0, buf.byteLength);
		let s = '';
		for (let i = 0; i < a.length; i++) s += String.fromCharCode(a[i]);
		if (this.encoding === 'utf-8' || this.encoding === 'utf8') { try { return decodeURIComponent(escape(s)); } catch (e) { return s; } }
		return s;
	}
}
G.TextEncoder = TextEncoder; G.TextDecoder = TextDecoder;
G.structuredClone = function (v) { return v === undefined ? undefined : JSON.parse(JSON.stringify(v)); };

/* ---------------- fetch / XHR ---------------- */
class Headers {
	constructor(init) {
		val(this, '__m', new Map());
		if (!init) return;
		if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
		else if (typeof init[Symbol.iterator] === 'function') for (const [k, v] of init) this.append(k, v);
		else for (const k of Object.keys(init)) this.append(k, init[k]);
	}
	append(k, v) { k = String(k).toLowerCase(); const o = this.__m.get(k); this.__m.set(k, o === undefined ? String(v) : o + ', ' + v); }
	set(k, v) { this.__m.set(String(k).toLowerCase(), String(v)); }
	get(k) { const v = this.__m.get(String(k).toLowerCase()); return v === undefined ? null : v; }
	has(k) { return this.__m.has(String(k).toLowerCase()); }
	delete(k) { this.__m.delete(String(k).toLowerCase()); }
	forEach(f, t) { for (const [k, v] of this.__m) f.call(t, v, k, this); }
	keys() { return this.__m.keys(); }
	values() { return this.__m.values(); }
	entries() { return this.__m.entries(); }
	[Symbol.iterator]() { return this.__m.entries(); }
}
class Blob {
	constructor(parts, opt) {
		val(this, '__s', (parts || []).map((p) => (p instanceof Blob ? p.__s : typeof p === 'string' ? p : String(p))).join(''));
		this.type = (opt && opt.type) || '';
	}
	get size() { return this.__s.length; }
	text() { return Promise.resolve(this.__s); }
	arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this.__s).buffer); }
	slice(a, b, t) { return new Blob([this.__s.slice(a, b)], { type: t }); }
}
class File extends Blob {
	constructor(parts, name, opt) { super(parts, opt); this.name = String(name); this.lastModified = Date.now(); }
}
class FileReader {
	constructor() { this.result = null; this.readyState = 0; this.onload = null; this.onloadend = null; this.onerror = null; }
	__done(r) { this.result = r; this.readyState = 2; setTimeout(() => { const ev = { target: this, type: 'load' }; if (this.onload) this.onload(ev); if (this.onloadend) this.onloadend(ev); }, 0); }
	readAsText(b) { this.__done(b.__s); }
	readAsDataURL(b) { this.__done('data:' + (b.type || 'application/octet-stream') + ';base64,' + G.btoa(unescape(encodeURIComponent(b.__s)))); }
	readAsArrayBuffer(b) { this.__done(new TextEncoder().encode(b.__s).buffer); }
	abort() {}
	addEventListener(t, f) { if (t === 'load') this.onload = f; else if (t === 'loadend') this.onloadend = f; else if (t === 'error') this.onerror = f; }
}
G.Headers = Headers; G.Blob = Blob; G.File = File; G.FileReader = FileReader;

class FormData {
	constructor(form) {
		val(this, '__l', []);
		if (!form) return;
		for (const el of qsa(form, 'input,select,textarea,button', false)) {
			const name = el.getAttribute('name');
			if (!name || el.hasAttribute('disabled')) continue;
			const tag = el.localName;
			const type = (el.getAttribute('type') || '').toLowerCase();
			if (tag === 'button' || type === 'submit' || type === 'button' || type === 'reset' || type === 'image' || type === 'file') continue;
			if ((type === 'checkbox' || type === 'radio') && !el.checked) continue;
			this.__l.push([name, (type === 'checkbox' || type === 'radio') && !el.hasAttribute('value') ? 'on' : String(el.value)]);
		}
	}
	append(k, v) { this.__l.push([String(k), v instanceof Blob ? v : String(v)]); }
	set(k, v) { this.delete(k); this.append(k, v); }
	get(k) { const p = this.__l.find((x) => x[0] === String(k)); return p ? p[1] : null; }
	getAll(k) { return this.__l.filter((x) => x[0] === String(k)).map((x) => x[1]); }
	has(k) { return this.__l.some((x) => x[0] === String(k)); }
	delete(k) { k = String(k); for (let i = this.__l.length - 1; i >= 0; i--) if (this.__l[i][0] === k) this.__l.splice(i, 1); }
	forEach(f, t) { for (const [k, v] of this.__l) f.call(t, v, k, this); }
	keys() { return this.__l.map((x) => x[0])[Symbol.iterator](); }
	values() { return this.__l.map((x) => x[1])[Symbol.iterator](); }
	entries() { return this.__l.map((x) => [x[0], x[1]])[Symbol.iterator](); }
	[Symbol.iterator]() { return this.entries(); }
}
G.FormData = FormData;

class AbortSignal extends EventTarget {
	constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
	throwIfAborted() { if (this.aborted) throw this.reason; }
	static abort(r) { const c = new AbortController(); c.abort(r); return c.signal; }
	static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new Error('TimeoutError')), ms); return c.signal; }
	static any() { return new AbortController().signal; }
}
class AbortController {
	constructor() { val(this, 'signal', new AbortSignal()); }
	abort(reason) {
		const s = this.signal;
		if (s.aborted) return;
		s.aborted = true;
		s.reason = reason === undefined ? new Error('AbortError') : reason;
		const ev = new Event('abort');
		if (typeof s.onabort === 'function') { try { s.onabort(ev); } catch (e) { reportError(e); } }
		s.dispatchEvent(ev);
	}
}
G.AbortController = AbortController; G.AbortSignal = AbortSignal;

function bodyToString(b) {
	if (b === undefined || b === null) return null;
	if (typeof b === 'string') return b;
	if (b instanceof URLSearchParams) return b.toString();
	if (b instanceof FormData) return new URLSearchParams(b.__l.map(([k, v]) => [k, typeof v === 'string' ? v : ''])).toString();
	if (b instanceof Blob) return b.__s;
	if (b instanceof ArrayBuffer || ArrayBuffer.isView(b)) return new TextDecoder().decode(b);
	return String(b);
}
class Response {
	constructor(body, init) {
		init = init || {};
		val(this, '__body', bodyToString(body) || '');
		this.status = init.status === undefined ? 200 : init.status;
		this.statusText = init.statusText || (this.status === 200 ? 'OK' : '');
		this.ok = this.status >= 200 && this.status < 300;
		this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
		this.url = init.url || '';
		this.type = 'basic';
		this.redirected = false;
		this.bodyUsed = false;
	}
	text() { this.bodyUsed = true; return Promise.resolve(this.__body); }
	json() { this.bodyUsed = true; try { return Promise.resolve(JSON.parse(this.__body)); } catch (e) { return Promise.reject(e); } }
	blob() { this.bodyUsed = true; return Promise.resolve(new Blob([this.__body], { type: this.headers.get('content-type') || '' })); }
	arrayBuffer() { this.bodyUsed = true; return Promise.resolve(new TextEncoder().encode(this.__body).buffer); }
	formData() { return Promise.resolve(new FormData()); }
	clone() { return new Response(this.__body, this); }
	static json(d, init) { const r = new Response(JSON.stringify(d), init); r.headers.set('content-type', 'application/json'); return r; }
	static error() { return new Response('', { status: 0 }); }
	static redirect(u, s) { return new Response('', { status: s || 302, headers: { location: u } }); }
}
class Request {
	constructor(input, init) {
		init = init || {};
		this.url = input instanceof Request ? input.url : String(input);
		this.method = (init.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();
		this.headers = new Headers(init.headers || (input instanceof Request ? input.headers : undefined));
		val(this, '__body', init.body !== undefined ? init.body : (input instanceof Request ? input.__body : null));
		this.signal = init.signal || null;
		this.credentials = init.credentials || 'same-origin';
		this.mode = init.mode || 'cors';
	}
	clone() { return new Request(this); }
}
G.Response = Response; G.Request = Request;

let fetchSeq = 0;
const FET = new Map();
/* request headers the page may set (cookies, host, referer, ... stay with the fetcher) */
const FORBIDDEN_HDR = /^(accept-charset|accept-encoding|access-control-request-.*|connection|content-length|cookie2?|date|dnt|expect|host|keep-alive|origin|referer|set-cookie|te|trailer|transfer-encoding|upgrade|user-agent|via|proxy-.*|sec-.*)$/;
function headerLines(h) {
	if (!h || typeof h.forEach !== 'function') return '';
	let s = '';
	h.forEach((v, k) => {
		k = String(k).toLowerCase(); v = String(v);
		if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(k) || FORBIDDEN_HDR.test(k) || /[\r\n\0]/.test(v)) return;
		s += k + ': ' + v + '\n';
	});
	return s;
}
function nativeFetch(url, method, body, headers, cb) {
	const id = ++fetchSeq;
	FET.set(id, cb);
	let ok;
	try { ok = N.fetch(id, String(url), method, body, headerLines(headers)); } catch (e) { FET.delete(id); throw e; }
	if (!ok) { FET.delete(id); return false; }
	return id;
}
function hookFetchDone(id, ok, status, url, body, ctype, err) {
	const cb = FET.get(id);
	if (!cb) return;
	FET.delete(id);
	cb(ok, status, url, body, ctype, err);
}
G.fetch = function (input, init) {
	const req = input instanceof Request && !init ? input : new Request(input, init);
	return new Promise((resolve, reject) => {
		const sig = req.signal;
		if (sig && sig.aborted) { reject(sig.reason); return; }
		let id;
		let settled = false;
		try {
			id = nativeFetch(req.url, req.method, bodyToString(req.__body), req.headers, (ok, status, url, body, ctype, err) => {
				if (settled) return;
				settled = true;
				if (!ok) { reject(new TypeError('Failed to fetch' + (err ? ': ' + err : ''))); return; }
				const h = new Headers();
				if (ctype) h.set('content-type', ctype);
				resolve(new Response(body, { status, headers: h, url: url || req.url }));
			});
		} catch (e) { reject(new TypeError('Failed to fetch: ' + e.message)); return; }
		if (id === false) { reject(new TypeError('Failed to fetch')); return; }
		if (sig) sig.addEventListener('abort', () => { if (!settled) { settled = true; FET.delete(id); reject(sig.reason); } });
	});
};

class XMLHttpRequest extends EventTarget {
	constructor() {
		super();
		this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = '';
		this.responseType = ''; this.responseURL = ''; this.timeout = 0; this.withCredentials = false;
		this.onreadystatechange = null; this.onload = null; this.onerror = null; this.onloadend = null;
		this.onprogress = null; this.onabort = null; this.ontimeout = null; this.onloadstart = null;
		this.upload = { addEventListener() {}, removeEventListener() {} };
		val(this, '__h', new Headers()); val(this, '__rh', new Headers()); val(this, '__id', 0);
	}
	open(method, url, async) {
		if (async === false) N.log(2, 'XMLHttpRequest: synchronous requests are not supported, running async');
		this.__method = String(method).toUpperCase();
		this.__url = String(url);
		this.readyState = 1;
		this.__fire('readystatechange');
	}
	setRequestHeader(k, v) { this.__h.set(k, v); }
	getResponseHeader(k) { return this.__rh.get(k); }
	getAllResponseHeaders() { let s = ''; this.__rh.forEach((v, k) => { s += k + ': ' + v + '\r\n'; }); return s; }
	overrideMimeType() {}
	__fire(type) {
		const ev = new ProgressEvent(type);
		const h = this['on' + type];
		if (typeof h === 'function') { try { h.call(this, ev); } catch (e) { reportError(e); } }
		this.dispatchEvent(ev);
	}
	send(body) {
		this.__fire('loadstart');
		const id = nativeFetch(this.__url, this.__method || 'GET', bodyToString(body), this.__h, (ok, status, url, text, ctype, err) => {
			this.__id = 0;
			if (!ok) { this.readyState = 4; this.__fire('readystatechange'); this.__fire('error'); this.__fire('loadend'); return; }
			this.status = status; this.statusText = 'OK'; this.responseURL = url || this.__url;
			if (ctype) this.__rh.set('content-type', ctype);
			this.responseText = text;
			if (this.responseType === 'json') { try { this.response = JSON.parse(text); } catch (e) { this.response = null; } }
			else if (this.responseType === 'document') this.response = new DOMParser().parseFromString(text, 'text/html');
			else if (this.responseType === 'arraybuffer') this.response = new TextEncoder().encode(text).buffer;
			else if (this.responseType === 'blob') this.response = new Blob([text], { type: ctype || '' });
			else this.response = text;
			this.readyState = 2; this.__fire('readystatechange');
			this.readyState = 3; this.__fire('readystatechange');
			this.readyState = 4; this.__fire('readystatechange');
			this.__fire('load'); this.__fire('loadend');
		});
		if (id === false) { setTimeout(() => { this.readyState = 4; this.__fire('readystatechange'); this.__fire('error'); this.__fire('loadend'); }, 0); return; }
		this.__id = id;
	}
	abort() { if (this.__id) { FET.delete(this.__id); this.__id = 0; this.readyState = 0; this.__fire('abort'); } }
}
Object.assign(XMLHttpRequest, { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 });
G.XMLHttpRequest = XMLHttpRequest;

/* ---------------- stubs ---------------- */
class IntersectionObserver {
	constructor(cb, opt) { val(this, '__cb', cb); this.root = (opt && opt.root) || null; this.rootMargin = ''; this.thresholds = [0]; }
	observe(el) {
		setTimeout(() => {
			try { this.__cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: zeroRect(), intersectionRect: zeroRect(), rootBounds: null, time: G.performance.now() }], this); }
			catch (e) { reportError(e); }
		}, 0);
	}
	unobserve() {} disconnect() {} takeRecords() { return []; }
}
/* ---- DOMException ---- */
const DOMEX_CODES = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5,
	NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12,
	InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15, TypeMismatchError: 17, SecurityError: 18,
	NetworkError: 19, AbortError: 20, URLMismatchError: 21, QuotaExceededError: 22, TimeoutError: 23, DataCloneError: 25 };
if (typeof G.DOMException !== 'function') {
	G.DOMException = class DOMException extends Error {
		constructor(message, name) {
			super(message === undefined ? '' : String(message));
			val(this, 'name', name === undefined ? 'Error' : String(name));
			val(this, 'code', DOMEX_CODES[this.name] || 0);
		}
	};
}
function domErr(msg, name) { return new G.DOMException(msg, name); }

/* ---- custom elements ----
 * Definitions live in CE (name) / CEC (constructor).  CES maps an element to
 * its definition once it is custom (false while being upgraded / failed).
 * Reactions are fired synchronously from the N.* mutation wrappers below,
 * which every JS-side DOM change goes through. */
const CE = new Map(), CEC = new Map(), CEW = new Map();
const CES = new WeakMap();
let ceUp = null, ceCount = 0;
const CE_RESERVED = new Set(['annotation-xml', 'color-profile', 'font-face', 'font-face-src', 'font-face-uri',
	'font-face-format', 'font-face-name', 'missing-glyph']);
function ceValidName(n) {
	return /^[a-z][-.0-9_a-z\u00b7\u00c0-\uffff]*-[-.0-9_a-z\u00b7\u00c0-\uffff]*$/.test(n) && !CE_RESERVED.has(n);
}
function ceCall(el, fn, args) {
	if (typeof fn !== 'function') return;
	try { fn.apply(el, args); } catch (e) { reportError(e); }
}
function ceUpgrade(el, d) {
	if (CES.has(el)) return;
	if (!d) { d = CE.get(lname(el)); if (!d || d.ext) return; }
	CES.set(el, false);
	const oldProto = Object.getPrototypeOf(el);
	Object.setPrototypeOf(el, d.proto);
	ceUp = { el, ctor: d.ctor };
	let ok = false;
	try {
		const r = Reflect.construct(d.ctor, []);
		if (r !== el) throw new TypeError("custom element constructor for '" + d.name + "' did not return the element");
		ok = true;
	} catch (e) { reportError(e); }
	ceUp = null;
	if (!ok) { if (Object.getPrototypeOf(el) === d.proto) Object.setPrototypeOf(el, oldProto); return; }
	CES.set(el, d);
	if (d.obs && d.attr) for (const a of N.attrs(el)) if (d.obs.has(a[0])) ceCall(el, d.attr, [a[0], null, a[1], null]);
	if (composedConnected(el)) ceCall(el, d.conn, []);
}
/* HTMLElement constructor: only reachable through `new` on a defined class or an upgrade */
function HTMLElementCtor() {
	const nt = new.target;
	if (!nt) illegal();
	if (ceUp && ceUp.ctor === nt) { const el = ceUp.el; ceUp = null; return el; }
	const d = CEC.get(nt);
	if (!d || d.ext) illegal();
	const el = N.createElement(d.name);
	Object.setPrototypeOf(el, nt.prototype);
	CES.set(el, d);
	return el;
}
function composedRoot(n) {
	for (;;) {
		let p;
		while ((p = N.parent(n))) n = p;
		const h = SRH.get(n);
		if (!h) return n;
		n = h;
	}
}
function composedConnected(n) { return N.nodeType(composedRoot(n)) === 9; }
/* elements in a subtree (inclusive), including open/closed shadow trees */
function ceSubtree(n, out) {
	if (N.nodeType(n) === 1) {
		out.push(n);
		const sr = SRS.get(n); if (sr) ceSubtree(sr, out);
	}
	if (!N.first(n)) return out;
	/* template contents are inert (libdom keeps them as children of <template>) */
	const tpl = N.collect(n, 'template', null, null, false);
	let skip = null;
	if (tpl.length) { skip = new Set(); for (const t of tpl) for (const e of N.collect(t, null, null, null, false)) skip.add(e); }
	if (N.nodeType(n) === 1 && lname(n) === 'template') return out;
	for (const e of N.collect(n, null, null, null, false)) {
		if (skip && skip.has(e)) continue;
		out.push(e);
		const sr = SRS.get(e); if (sr) ceSubtree(sr, out);
	}
	return out;
}
function ceScan(root) {
	if (!ceCount) return;
	for (const e of ceSubtree(root, [])) if (!CES.has(e)) ceUpgrade(e);
}
function ceConnectedTree(n) {
	if (!ceCount) return;
	for (const e of ceSubtree(n, [])) {
		const st = CES.get(e);
		if (st) ceCall(e, st.conn, []);
		else if (st === undefined) ceUpgrade(e);
	}
}
/* custom elements in a connected subtree that have a disconnectedCallback */
function ceDiscList(n) {
	if (!ceCount || !composedConnected(n)) return null;
	const out = [];
	for (const e of ceSubtree(n, [])) { const st = CES.get(e); if (st && st.disc) out.push(e); }
	return out.length ? out : null;
}
function ceFireDisc(list) { if (list) for (const e of list) ceCall(e, CES.get(e).disc, []); }

class CustomElementRegistry {
	define(name, ctor, opts) {
		name = String(name);
		if (typeof ctor !== 'function') throw new TypeError('customElements.define: constructor is not a function');
		if (!ceValidName(name)) throw domErr("'" + name + "' is not a valid custom element name", 'SyntaxError');
		if (CE.has(name)) throw domErr("the name '" + name + "' has already been used with this registry", 'NotSupportedError');
		if (CEC.has(ctor)) throw domErr('this constructor has already been used with this registry', 'NotSupportedError');
		const proto = ctor.prototype;
		if (proto === null || typeof proto !== 'object') throw new TypeError('customElements.define: prototype is not an object');
		const cb = (k) => { const f = proto[k]; if (f !== undefined && typeof f !== 'function') throw new TypeError(k + ' is not a function'); return f; };
		const d = { name, ctor, proto, conn: cb('connectedCallback'), disc: cb('disconnectedCallback'),
			adopt: cb('adoptedCallback'), attr: cb('attributeChangedCallback'), obs: null,
			ext: opts && opts.extends ? String(opts.extends).toLowerCase() : null,
			formAssociated: !!ctor.formAssociated };
		if (d.attr) {
			const oa = ctor.observedAttributes;
			if (oa != null) d.obs = new Set(Array.from(oa, (x) => String(x)));
		}
		CE.set(name, d); CEC.set(ctor, d); ceCount++;
		if (!d.ext && G.document) {
			for (const e of ceSubtree(G.document, [])) if (!CES.has(e) && lname(e) === name) ceUpgrade(e, d);
		}
		const w = CEW.get(name);
		if (w) { CEW.delete(name); w.res(ctor); }
	}
	get(name) { const d = CE.get(String(name)); return d ? d.ctor : undefined; }
	getName(ctor) { const d = CEC.get(ctor); return d ? d.name : null; }
	whenDefined(name) {
		name = String(name);
		if (!ceValidName(name)) return Promise.reject(domErr("'" + name + "' is not a valid custom element name", 'SyntaxError'));
		const d = CE.get(name);
		if (d) return Promise.resolve(d.ctor);
		let w = CEW.get(name);
		if (!w) { w = {}; w.p = new Promise((r) => { w.res = r; }); CEW.set(name, w); }
		return w.p;
	}
	upgrade(root) { if (isNode(root)) for (const e of ceSubtree(root, [])) if (!CES.has(e)) ceUpgrade(e); }
}
G.CustomElementRegistry = CustomElementRegistry;
G.customElements = new CustomElementRegistry();

/* ---- MutationObserver ---- */
const MOREG = new WeakMap();	/* node -> [{mo, o}] */
let moCount = 0, moQueued = false;
const moPending = [];
class MutationRecord {}
function moList(a) { return makeList(a ? a.slice() : []); }
function moQueue(target, type, info) {
	let seen = null;
	for (let n = target; n; n = N.parent(n)) {
		const regs = MOREG.get(n);
		if (!regs) continue;
		for (const r of regs) {
			const o = r.o;
			if (n !== target && !o.subtree) continue;
			if (type === 'attributes') {
				if (!o.attributes) continue;
				if (o.attributeFilter && !o.attributeFilter.includes(info.name)) continue;
			} else if (type === 'characterData') { if (!o.characterData) continue; }
			else if (!o.childList) continue;
			if (!seen) seen = new Map();
			const keepOld = (type === 'attributes' && o.attributeOldValue) || (type === 'characterData' && o.characterDataOldValue);
			if (seen.has(r.mo)) { if (keepOld) seen.set(r.mo, true); continue; }
			seen.set(r.mo, !!keepOld);
		}
	}
	if (!seen) return;
	for (const [mo, keepOld] of seen) {
		const rec = Object.create(MutationRecord.prototype);
		rec.type = type; rec.target = target;
		rec.addedNodes = moList(info.added); rec.removedNodes = moList(info.removed);
		rec.previousSibling = info.prev || null; rec.nextSibling = info.next || null;
		rec.attributeName = type === 'attributes' ? info.name : null; rec.attributeNamespace = null;
		rec.oldValue = keepOld ? (info.old === undefined ? null : info.old) : null;
		mo.__q.push(rec);
		if (!mo.__pend) { mo.__pend = true; moPending.push(mo); }
	}
	if (!moQueued) { moQueued = true; Promise.resolve().then(moDeliver); }
}
function moDeliver() {
	moQueued = false;
	const list = moPending.splice(0);
	for (const mo of list) {
		mo.__pend = false;
		const recs = mo.__q.splice(0);
		if (recs.length) { try { mo.__cb.call(mo, recs, mo); } catch (e) { reportError(e); } }
	}
}
class MutationObserver {
	constructor(cb) {
		if (typeof cb !== 'function') throw new TypeError('MutationObserver: callback is not a function');
		val(this, '__cb', cb); val(this, '__q', []); val(this, '__t', []); val(this, '__pend', false);
	}
	observe(target, opts) {
		if (!isNode(target)) throw new TypeError('MutationObserver.observe: target is not a Node');
		const o = Object.assign({}, opts || {});
		if (o.attributeFilter !== undefined) o.attributeFilter = Array.from(o.attributeFilter, (x) => String(x));
		if ((o.attributeOldValue || o.attributeFilter) && o.attributes === undefined) o.attributes = true;
		if (o.characterDataOldValue && o.characterData === undefined) o.characterData = true;
		if (!o.childList && !o.attributes && !o.characterData)
			throw new TypeError("MutationObserver.observe: one of 'childList', 'attributes' or 'characterData' must be true");
		let regs = MOREG.get(target);
		if (!regs) { regs = []; MOREG.set(target, regs); }
		const r = regs.find((x) => x.mo === this);
		if (r) { r.o = o; return; }
		regs.push({ mo: this, o }); this.__t.push(target); moCount++;
	}
	disconnect() {
		for (const t of this.__t) {
			const regs = MOREG.get(t); if (!regs) continue;
			const i = regs.findIndex((x) => x.mo === this);
			if (i >= 0) { regs.splice(i, 1); moCount--; }
		}
		this.__t.length = 0; this.__q.length = 0;
	}
	takeRecords() { return this.__q.splice(0); }
}

/* ---- reaction-aware wrappers around the native mutation primitives ---- */
{
	const nAppend = N.append, nIns = N.insertBefore, nRem = N.remove, nRep = N.replace;
	const nSetAttr = N.setAttr, nRemAttr = N.removeAttr, nSetText = N.setText, nSetValue = N.setValue;
	const kids = (x) => (N.nodeType(x) === 11 ? childArray(x, false) : [x]);
	/* x is about to leave its old parent (a move): disconnect + MO record there */
	function detachPre(x) {
		const op = N.parent(x);
		if (!op) return null;
		const disc = ceDiscList(x);
		if (moCount) moQueue(op, 'childList', { removed: [x], prev: N.prev(x), next: N.next(x) });
		return disc;
	}
	function inserted(p, nodes, prev, next) {
		if (moCount && nodes.length) moQueue(p, 'childList', { added: nodes, prev, next });
		if (ceCount && composedConnected(p)) for (const x of nodes) ceConnectedTree(x);
	}
	function doInsert(p, c, ref, fn) {
		if (!(ceCount || moCount) || !isNode(c) || !isNode(p)) return fn();
		const frag = N.nodeType(c) === 11;
		const nodes = kids(c);
		let disc = null;
		if (frag) { if (moCount && nodes.length) moQueue(c, 'childList', { removed: nodes }); }
		else disc = detachPre(c);
		const r = fn();
		ceFireDisc(disc);
		const prev = nodes.length ? N.prev(nodes[0]) : null;
		inserted(p, nodes, prev, ref || null);
		return r;
	}
	N.append = function (p, c) { return doInsert(p, c, null, () => nAppend(p, c)); };
	N.insertBefore = function (p, c, ref) { return doInsert(p, c, ref, () => nIns(p, c, ref)); };
	N.remove = function (p, c) {
		if (!(ceCount || moCount) || !isNode(c)) return nRem(p, c);
		const disc = ceDiscList(c);
		const prev = N.prev(c), next = N.next(c);
		const r = nRem(p, c);
		if (moCount) moQueue(p, 'childList', { removed: [c], prev, next });
		ceFireDisc(disc);
		return r;
	};
	N.replace = function (p, nc, old) {
		if (!(ceCount || moCount) || !isNode(nc) || !isNode(old)) return nRep(p, nc, old);
		const nodes = kids(nc);
		const disc1 = N.nodeType(nc) === 11 ? null : detachPre(nc);
		const disc2 = ceDiscList(old);
		const prev = N.prev(old), next = N.next(old);
		const r = nRep(p, nc, old);
		if (moCount) moQueue(p, 'childList', { added: nodes, removed: [old], prev: prev === nc ? N.prev(nodes[0]) : prev, next });
		ceFireDisc(disc1); ceFireDisc(disc2);
		if (ceCount && composedConnected(p)) for (const x of nodes) ceConnectedTree(x);
		return r;
	};
	function attrChanged(el, name, old, nv) {
		if (moCount) moQueue(el, 'attributes', { name, old });
		if (ceCount) {
			const st = CES.get(el);
			if (st && st.obs && st.obs.has(name)) ceCall(el, st.attr, [name, old, nv, null]);
		}
	}
	N.setAttr = function (el, name, v) {
		if (!(ceCount || moCount)) return nSetAttr(el, name, v);
		name = String(name).toLowerCase();
		const old = N.getAttr(el, name);
		const r = nSetAttr(el, name, v);
		attrChanged(el, name, old, N.getAttr(el, name));
		return r;
	};
	N.removeAttr = function (el, name) {
		if (!(ceCount || moCount)) return nRemAttr(el, name);
		name = String(name).toLowerCase();
		const old = N.getAttr(el, name);
		const r = nRemAttr(el, name);
		if (old !== null) attrChanged(el, name, old, null);
		return r;
	};
	N.setText = function (n, s) {
		const t = N.nodeType(n);
		if (!(ceCount || moCount) || (t !== 1 && t !== 11)) {
			if ((t === 3 || t === 8) && moCount) moQueue(n, 'characterData', { old: N.nodeValue(n) });
			return nSetText(n, s);
		}
		const removed = childArray(n, false);
		const disc = [];
		for (const c of removed) { const l = ceDiscList(c); if (l) disc.push(...l); }
		const r = nSetText(n, s);
		if (moCount) {
			const added = childArray(n, false);
			if (removed.length || added.length) moQueue(n, 'childList', { added, removed });
		}
		ceFireDisc(disc);
		return r;
	};
	N.setValue = function (n, s) {
		if (moCount) moQueue(n, 'characterData', { old: N.nodeValue(n) });
		return nSetValue(n, s);
	};
}

/* ---- shadow DOM (minimal: the tree exists for scripts; it is not rendered,
 * the light DOM stays as the rendered fallback) ---- */
const ShadowRoot = sub('ShadowRoot', DocumentFragment);
{
	const SP = ShadowRoot.prototype;
	def(SP, 'host', function () { return SRH.get(this) || null; });
	def(SP, 'mode', function () { const m = SRM.get(this); return m ? m.mode : 'open'; });
	def(SP, 'delegatesFocus', function () { const m = SRM.get(this); return !!(m && m.delegatesFocus); });
	def(SP, 'slotAssignment', function () { const m = SRM.get(this); return (m && m.slotAssignment) || 'named'; });
	def(SP, 'clonable', function () { return false; });
	def(SP, 'serializable', function () { return false; });
	def(SP, 'activeElement', function () { return null; });
	def(SP, 'fullscreenElement', function () { return null; });
	def(SP, 'pointerLockElement', function () { return null; });
	def(SP, 'styleSheets', function () { return []; });
	def(SP, 'adoptedStyleSheets', function () { const m = SRM.get(this); return m ? m.sheets : []; },
		function (v) { const m = SRM.get(this); if (m) m.sheets = Array.from(v || []); });
	const ih = Object.getOwnPropertyDescriptor(EP, 'innerHTML');
	Object.defineProperty(SP, 'innerHTML', {
		get() { return serializeChildren(this); },
		set(v) { let c; while ((c = N.first(this))) N.remove(this, c); parseHTMLInto(this, v == null ? '' : String(v)); },
		configurable: true,
	});
	void ih;
	SP.setHTMLUnsafe = function (h) { this.innerHTML = h; };
	SP.getHTML = function () { return serializeChildren(this); };
	SP.elementFromPoint = function () { return null; };
	SP.elementsFromPoint = function () { return []; };
	SP.getSelection = function () { return G.getSelection ? G.getSelection() : null; };
}
EP.attachShadow = function (init) {
	if (SRS.has(this)) throw domErr("Element already has a shadow root", 'NotSupportedError');
	const mode = init && init.mode === 'closed' ? 'closed' : 'open';
	const sr = N.createFragment();
	Object.setPrototypeOf(sr, ShadowRoot.prototype);
	SRH.set(sr, this); SRS.set(this, sr);
	SRM.set(sr, { mode, delegatesFocus: !!(init && init.delegatesFocus), slotAssignment: init && init.slotAssignment, sheets: [] });
	return sr;
};
def(EP, 'shadowRoot', function () { const sr = SRS.get(this); return sr && SRM.get(sr).mode === 'open' ? sr : null; });
def(EP, 'assignedSlot', function () { return null; });
EP.getHTML = function () { return serializeChildren(this); };
EP.setHTMLUnsafe = function (h) { this.innerHTML = h; };
DP.adoptedStyleSheets = [];
if (!G.HTMLSlotElement) {
	htmlClass('HTMLSlotElement', ['slot'], (P) => {
		reflect(P, 'name');
		P.assignedNodes = function () { return []; };
		P.assignedElements = function () { return []; };
		P.assign = function () {};
	});
}
/* composed path / host retargeting is not modelled; getRootNode({composed}) crosses hosts */
NP.getRootNode = function (opt) {
	if (opt && opt.composed) return composedRoot(this);
	let n = this, p; while ((p = N.parent(n))) n = p; return n;
};
def(NP, 'isConnected', function () { return composedConnected(this); });

/* constructable stylesheets (not applied to rendering) */
if (!G.CSSStyleSheet || !('replaceSync' in G.CSSStyleSheet.prototype)) {
	G.CSSStyleSheet = class CSSStyleSheet {
		constructor(opt) { this.cssRules = []; this.disabled = !!(opt && opt.disabled); this.media = (opt && opt.media) || ''; this.__text = ''; }
		get rules() { return this.cssRules; }
		replaceSync(t) { this.__text = String(t); }
		replace(t) { this.__text = String(t); return Promise.resolve(this); }
		insertRule(r, i) { i = i === undefined ? 0 : i; this.cssRules.splice(i, 0, { cssText: String(r) }); return i; }
		deleteRule(i) { this.cssRules.splice(i, 1); }
		addRule() { return -1; }
		removeRule(i) { this.deleteRule(i || 0); }
	};
}

/* ---- ElementInternals (form-associated bits are stubs) ---- */
class CustomStateSet extends Set {}
class ElementInternals {
	constructor() { illegal(); }
	get shadowRoot() { const sr = SRS.get(this.__el); return sr || null; }
	get form() { return this.__el.closest ? this.__el.closest('form') : null; }
	get labels() { return makeList([]); }
	get willValidate() { return false; }
	get validity() { return this.__v; }
	get validationMessage() { return this.__m; }
	setFormValue(v) { val(this, '__fv', v); }
	setValidity(flags, msg) {
		const v = Object.assign({ valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false,
			tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false,
			customError: false }, flags || {});
		v.valid = !Object.keys(v).some((k) => v[k] === true);
		val(this, '__v', v); val(this, '__m', v.valid ? '' : String(msg || ''));
	}
	checkValidity() { return this.__v.valid; }
	reportValidity() { return this.__v.valid; }
}
HP.attachInternals = function () {
	if (EIS.has(this)) throw domErr('ElementInternals already attached', 'NotSupportedError');
	const i = Object.create(ElementInternals.prototype);
	val(i, '__el', this); val(i, '__v', { valid: true }); val(i, '__m', '');
	val(i, 'states', new CustomStateSet());
	i.role = null;
	EIS.set(this, i);
	return i;
};
G.ElementInternals = ElementInternals; G.CustomStateSet = CustomStateSet;
G.ShadowRoot = ShadowRoot; G.MutationRecord = MutationRecord;
class ResizeObserver { constructor() {} observe() {} unobserve() {} disconnect() {} }
class PerformanceObserver { constructor() {} observe() {} disconnect() {} static get supportedEntryTypes() { return []; } }
G.IntersectionObserver = IntersectionObserver; G.MutationObserver = MutationObserver;
G.WebKitMutationObserver = MutationObserver;
G.ResizeObserver = ResizeObserver; G.PerformanceObserver = PerformanceObserver;
G.BroadcastChannel = class BroadcastChannel extends EventTarget { constructor(n) { super(); this.name = n; } postMessage() {} close() {} };
G.MessageChannel = class MessageChannel { constructor() { this.port1 = { postMessage() {}, onmessage: null, close() {}, start() {}, addEventListener() {} }; this.port2 = this.port1; } };
G.CSS = { supports() { return false; }, escape: cssEscape };
G.crypto = {
	getRandomValues(a) {
		for (let i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 4294967296);
		return a;
	},
	randomUUID() {
		const h = [];
		for (let i = 0; i < 16; i++) h.push(Math.floor(Math.random() * 256));
		h[6] = (h[6] & 15) | 64; h[8] = (h[8] & 63) | 128;
		const x = h.map((b) => (b < 16 ? '0' : '') + b.toString(16)).join('');
		return x.slice(0, 8) + '-' + x.slice(8, 12) + '-' + x.slice(12, 16) + '-' + x.slice(16, 20) + '-' + x.slice(20);
	},
	subtle: {},
};
G.Worker = function Worker() { throw new Error('Workers are not supported'); };
G.WebSocket = function WebSocket() { throw new Error('WebSocket is not supported'); };
G.Notification = { permission: 'denied', requestPermission() { return Promise.resolve('denied'); } };

/* ---------------- minimal Intl (QuickJS has none) ---------------- */
if (typeof G.Intl === 'undefined') {
	const MON = ['January','February','March','April','May','June','July','August','September','October','November','December'];
	const DAY = ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];
	const p2 = n => (n < 10 ? '0' : '') + n;
	const ro = (o, d) => Object.assign({ locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' }, d, o || {});
	class DateTimeFormat {
		constructor(l, o) { this._o = ro(o); }
		resolvedOptions() { return this._o; }
		formatToParts(d) {
			d = d === undefined ? new Date() : new Date(d);
			const o = this._o, parts = [], add = (t, v) => { if (parts.length) parts.push({ type: 'literal', value: t === 'year' && (o.month === 'long' || o.month === 'short') ? ', ' : (t === 'hour' ? ', ' : (t === 'minute' || t === 'second' ? ':' : (o.month === 'short' || o.month === 'long' ? ' ' : '/'))) }); parts.push({ type: t, value: v }); };
			const none = !o.year && !o.month && !o.day && !o.hour && !o.minute && !o.weekday && !o.dateStyle && !o.timeStyle;
			if (o.weekday) add('weekday', o.weekday === 'long' ? DAY[d.getDay()] : DAY[d.getDay()].slice(0, 3));
			if (o.month || none || o.dateStyle) add('month', o.month === 'long' ? MON[d.getMonth()] : o.month === 'short' ? MON[d.getMonth()].slice(0, 3) : o.month === '2-digit' ? p2(d.getMonth() + 1) : String(d.getMonth() + 1));
			if (o.day || none || o.dateStyle) add('day', o.day === '2-digit' ? p2(d.getDate()) : String(d.getDate()));
			if (o.year || none || o.dateStyle) add('year', o.year === '2-digit' ? p2(d.getFullYear() % 100) : String(d.getFullYear()));
			if (o.hour || o.timeStyle) { const h = d.getHours(); add('hour', o.hour12 === false || o.hourCycle === 'h23' ? p2(h) : String(h % 12 || 12)); }
			if (o.minute || o.timeStyle) add('minute', p2(d.getMinutes()));
			if (o.second) add('second', p2(d.getSeconds()));
			if ((o.hour || o.timeStyle) && o.hour12 !== false && o.hourCycle !== 'h23') { parts.push({ type: 'literal', value: ' ' }, { type: 'dayPeriod', value: d.getHours() < 12 ? 'AM' : 'PM' }); }
			return parts;
		}
		format(d) { return this.formatToParts(d).map(p => p.value).join(''); }
		formatRange(a, b) { return this.format(a) + ' – ' + this.format(b); }
		static supportedLocalesOf(l) { return [].concat(l || []); }
	}
	class NumberFormat {
		constructor(l, o) { this._o = ro(o, { style: 'decimal', minimumFractionDigits: 0, maximumFractionDigits: 3 }); }
		resolvedOptions() { return this._o; }
		format(n) {
			const o = this._o; n = Number(n);
			if (!isFinite(n)) return String(n);
			if (o.notation === 'compact') { const a = Math.abs(n); const u = a >= 1e9 ? ['B', 1e9] : a >= 1e6 ? ['M', 1e6] : a >= 1e3 ? ['K', 1e3] : null; if (u) { const v = n / u[1]; return (Math.abs(v) < 10 ? Math.round(v * 10) / 10 : Math.round(v)) + u[0]; } }
			let v = o.style === 'percent' ? n * 100 : n;
			const mx = o.maximumFractionDigits, mn = Math.min(o.minimumFractionDigits, mx);
			let s = v.toFixed(mx);
			if (s.indexOf('.') >= 0) { s = s.replace(/0+$/, ''); const f = s.split('.')[1] || ''; if (f.length < mn) s += '0'.repeat(mn - f.length); s = s.replace(/\.$/, ''); }
			const [i, f] = s.split('.');
			s = (o.useGrouping === false ? i : i.replace(/\B(?=(\d{3})+(?!\d))/g, ',')) + (f ? '.' + f : '');
			if (o.style === 'percent') s += '%';
			if (o.style === 'currency') s = (o.currency === 'USD' ? '$' : o.currency === 'EUR' ? '€' : (o.currency || '') + ' ') + s;
			if (o.style === 'unit' && o.unit) s += ' ' + o.unit;
			return s;
		}
		formatToParts(n) { return [{ type: 'integer', value: this.format(n) }]; }
		static supportedLocalesOf(l) { return [].concat(l || []); }
	}
	class PluralRules {
		constructor(l, o) { this._o = ro(o, { type: 'cardinal' }); }
		resolvedOptions() { return this._o; }
		select(n) { if (this._o.type === 'ordinal') { const a = n % 10, b = n % 100; return a === 1 && b !== 11 ? 'one' : a === 2 && b !== 12 ? 'two' : a === 3 && b !== 13 ? 'few' : 'other'; } return n === 1 ? 'one' : 'other'; }
		static supportedLocalesOf(l) { return [].concat(l || []); }
	}
	class RelativeTimeFormat {
		constructor(l, o) { this._o = ro(o, { numeric: 'always', style: 'long' }); }
		resolvedOptions() { return this._o; }
		format(v, u) { u = String(u).replace(/s$/, ''); v = Number(v); if (this._o.numeric === 'auto') { if (v === 0 && u === 'day') return 'today'; if (v === -1 && u === 'day') return 'yesterday'; if (v === 1 && u === 'day') return 'tomorrow'; if (v === 0) return 'this ' + u; } const a = Math.abs(v), w = a + ' ' + u + (a === 1 ? '' : 's'); return v < 0 || Object.is(v, -0) ? w + ' ago' : 'in ' + w; }
		formatToParts(v, u) { return [{ type: 'literal', value: this.format(v, u) }]; }
		static supportedLocalesOf(l) { return [].concat(l || []); }
	}
	class Collator {
		constructor(l, o) { this._o = ro(o, { sensitivity: 'variant' }); }
		resolvedOptions() { return this._o; }
		compare = (a, b) => { a = String(a); b = String(b); if (this._o.sensitivity === 'base' || this._o.sensitivity === 'accent') { a = a.toLowerCase(); b = b.toLowerCase(); } if (this._o.numeric) { const na = parseFloat(a), nb = parseFloat(b); if (!isNaN(na) && !isNaN(nb) && na !== nb) return na < nb ? -1 : 1; } return a < b ? -1 : a > b ? 1 : 0; };
		static supportedLocalesOf(l) { return [].concat(l || []); }
	}
	class ListFormat {
		constructor(l, o) { this._o = ro(o, { type: 'conjunction' }); }
		format(a) { a = Array.from(a); const w = this._o.type === 'disjunction' ? ' or ' : ' and '; return a.length < 2 ? a.join('') : a.length === 2 ? a.join(w) : a.slice(0, -1).join(', ') + ',' + w + a[a.length - 1]; }
	}
	class Segmenter {
		constructor(l, o) { this._o = ro(o, { granularity: 'grapheme' }); }
		segment(s) { s = String(s); const g = this._o.granularity; const out = []; if (g === 'word') { const re = /\w+|\W+/g; let m; while ((m = re.exec(s))) out.push({ segment: m[0], index: m.index, input: s, isWordLike: /\w/.test(m[0]) }); } else { let i = 0; for (const c of s) { out.push({ segment: c, index: i, input: s }); i += c.length; } } out.containing = i => out.find(x => i >= x.index && i < x.index + x.segment.length); return out; }
	}
	class Locale { constructor(t) { this.baseName = String(t || 'en-US'); this.language = this.baseName.split('-')[0]; } toString() { return this.baseName; } maximize() { return this; } }
	G.Intl = { DateTimeFormat, NumberFormat, PluralRules, RelativeTimeFormat, Collator, ListFormat, Segmenter, Locale,
		getCanonicalLocales(l) { return [].concat(l || []); }, supportedValuesOf() { return []; } };
	const nf = Number.prototype.toLocaleString;
	Number.prototype.toLocaleString = function (l, o) { return new NumberFormat(l, o).format(this); };
	Date.prototype.toLocaleDateString = function (l, o) { return new DateTimeFormat(l, o || { year: 'numeric', month: 'numeric', day: 'numeric' }).format(this); };
	Date.prototype.toLocaleTimeString = function (l, o) { return new DateTimeFormat(l, o || { hour: 'numeric', minute: '2-digit' }).format(this); };
	String.prototype.localeCompare = function (b) { return new Collator().compare(this, b); };
	void nf;
}


/* ---------------- hooks for the C side ---------------- */
function proto(type, tag) {
	switch (type) {
	case 1: return ELPROTO[tag] || (tag.indexOf('-') > 0 ? HTMLElement.prototype : G.HTMLUnknownElement.prototype);
	case 3: case 4: return Text.prototype;
	case 8: return Comment.prototype;
	case 9: return HTMLDocument.prototype;
	case 10: return DocumentType.prototype;
	case 11: return DocumentFragment.prototype;
	default: return Node.prototype;
	}
}
G.__nsHooks = { proto, dispatch: hookDispatch, window: hookWindow, timer: hookTimer, fetchdone: hookFetchDone, evproto };
})(globalThis.__ns);
