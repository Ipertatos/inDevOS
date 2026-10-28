/*
 * CSS Grid layout for NetSurf 3.11 (box patch).
 *
 * Grid containers use the flex box types (BOX_FLEX / BOX_INLINE_FLEX), so
 * box normalisation, redraw and the box inspector need no changes;
 * layout_flex() hands containers whose computed display is grid or
 * inline-grid to layout_grid().
 *
 * Supported: grid-template-columns/-rows (lengths, %, fr, auto,
 * min-content, max-content, minmax(), fit-content(), repeat(N, ...),
 * repeat(auto-fill|auto-fit, ...), [line names] ignored),
 * grid-template-areas with grid-area / grid-row / grid-column names,
 * grid-auto-columns/-rows, grid-auto-flow row|column [dense],
 * line numbers (incl. negative) and span placement, gap/row-gap/column-gap,
 * justify-items/-self, align-items/-self, justify-content, align-content.
 * Auto rows are sized to the laid-out height of their items.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <stdio.h>

#include "utils/log.h"
#include "utils/utils.h"

#include "html/box.h"
#include "html/html.h"
#include "html/private.h"
#include "html/box_inspect.h"
#include "html/layout_internal.h"

#define G_MAX_TRACKS 1000
#define G_INF (-1)

enum gsz { GS_AUTO, GS_LEN, GS_PCT, GS_FR, GS_MINC, GS_MAXC };

struct gsize {
	enum gsz type;
	float v;
	css_unit unit;
};

struct gdef {
	struct gsize min, max;
};

struct gtrack {
	struct gdef def;
	int base;
	int limit;
	int size;
	int pos;
	bool collapsible;
	bool used;
	bool collapsed;
};

struct gtemplate {
	struct gdef *defs;
	int n;
	int alloc;
	int rep_start;   /* auto repeat: first track index */
	int rep_len;     /* auto repeat: tracks per repetition, 0 = none */
	bool rep_fit;    /* auto-fit (else auto-fill) */
};

enum gline_type { GL_AUTO, GL_LINE, GL_SPAN, GL_NAME };

struct gline {
	enum gline_type type;
	int n;
	char name[48];
};

struct garea {
	char name[48];
	int r0, r1, c0, c1;
};

struct gitem {
	struct box *b;
	int r0, rspan, c0, cspan;
	bool rdef, cdef;
	int minc, maxc;  /* outer min/max-content widths */
	int h;           /* outer height after layout */
};

struct grid {
	struct box *box;
	html_content *content;
	const css_unit_ctx *uctx;
	struct gitem *items;
	int nitems;
	struct gtrack *cols, *rows;
	int ncols, nrows;
	int col_gap, row_gap;
	bool column_flow, dense;
	struct garea areas[32];
	int nareas, area_rows, area_cols;
};

/* ------------------------------------------------------------------ */
/* string helpers */

static const char *g_str(lwc_string *s, char *buf, size_t size)
{
	size_t n;
	if (s == NULL)
		return NULL;
	n = lwc_string_length(s);
	if (n >= size)
		n = size - 1;
	memcpy(buf, lwc_string_data(s), n);
	buf[n] = '\0';
	return buf;
}

static void g_ws(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n')
		(*p)++;
}

static bool g_word(const char *p, const char *w)
{
	size_t l = strlen(w);
	return strncasecmp(p, w, l) == 0;
}

static bool g_identc(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_';
}

static void g_skip_parens(const char **p)
{
	int depth = 0;
	while (**p != '\0') {
		if (**p == '(')
			depth++;
		else if (**p == ')') {
			depth--;
			if (depth <= 0) {
				(*p)++;
				return;
			}
		}
		(*p)++;
	}
}

static css_unit g_unit(const char *u, size_t l, bool *ok)
{
	static const struct { const char *n; css_unit u; } map[] = {
		{ "px", CSS_UNIT_PX }, { "em", CSS_UNIT_EM },
		{ "ex", CSS_UNIT_EX }, { "ch", CSS_UNIT_CH },
		{ "rem", CSS_UNIT_REM }, { "vh", CSS_UNIT_VH },
		{ "vw", CSS_UNIT_VW }, { "vmin", CSS_UNIT_VMIN },
		{ "vmax", CSS_UNIT_VMAX }, { "mm", CSS_UNIT_MM },
		{ "cm", CSS_UNIT_CM }, { "in", CSS_UNIT_IN },
		{ "pt", CSS_UNIT_PT }, { "pc", CSS_UNIT_PC },
		{ "q", CSS_UNIT_Q }, { "lh", CSS_UNIT_LH },
	};
	*ok = true;
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (strlen(map[i].n) == l && strncasecmp(u, map[i].n, l) == 0)
			return map[i].u;
	*ok = false;
	return CSS_UNIT_PX;
}

/* parse one <track-breadth> */
static bool g_parse_size(const char **p, struct gsize *s)
{
	const char *q;
	g_ws(p);
	q = *p;
	s->type = GS_AUTO;
	s->v = 0;
	s->unit = CSS_UNIT_PX;
	if ((*q >= '0' && *q <= '9') || *q == '.' ||
			((*q == '-' || *q == '+') &&
			 ((q[1] >= '0' && q[1] <= '9') || q[1] == '.'))) {
		char *end;
		const char *u;
		bool ok;
		s->v = strtof(q, &end);
		u = end;
		while (g_identc(*end) || *end == '%')
			end++;
		*p = end;
		if (end - u == 1 && *u == '%') {
			s->type = GS_PCT;
		} else if (end - u == 2 && strncasecmp(u, "fr", 2) == 0) {
			s->type = GS_FR;
		} else if (end == u) {
			s->type = GS_LEN;   /* unitless (0) */
		} else {
			s->unit = g_unit(u, end - u, &ok);
			s->type = ok ? GS_LEN : GS_AUTO;
		}
		return true;
	}
	if (g_word(q, "calc(") || g_word(q, "min(") || g_word(q, "max(") ||
			g_word(q, "clamp(") || g_word(q, "var(")) {
		g_skip_parens(p);
		return true;
	}
	if (!g_identc(*q))
		return false;
	while (g_identc(**p))
		(*p)++;
	if (g_word(q, "min-content"))
		s->type = GS_MINC;
	else if (g_word(q, "max-content"))
		s->type = GS_MAXC;
	return true;
}

/* parse one <track-size> */
static bool g_parse_track(const char **p, struct gdef *d)
{
	struct gsize s;
	g_ws(p);
	if (g_word(*p, "minmax(")) {
		*p += 7;
		if (!g_parse_size(p, &d->min))
			return false;
		g_ws(p);
		if (**p == ',')
			(*p)++;
		if (!g_parse_size(p, &d->max))
			return false;
		g_ws(p);
		if (**p == ')')
			(*p)++;
		if (d->min.type == GS_FR)
			d->min.type = GS_AUTO;
		return true;
	}
	if (g_word(*p, "fit-content(")) {
		*p += 12;
		if (!g_parse_size(p, &d->max))
			return false;
		g_ws(p);
		if (**p == ')')
			(*p)++;
		d->min.type = GS_AUTO;
		return true;
	}
	if (!g_parse_size(p, &s))
		return false;
	if (s.type == GS_FR) {
		d->min.type = GS_AUTO;
		d->min.v = 0;
		d->max = s;
	} else {
		d->min = s;
		d->max = s;
	}
	return true;
}

static bool g_tpl_add(struct gtemplate *t, const struct gdef *d)
{
	if (t->n >= G_MAX_TRACKS)
		return false;
	if (t->n == t->alloc) {
		int na = t->alloc ? t->alloc * 2 : 16;
		struct gdef *nd = realloc(t->defs, na * sizeof(*nd));
		if (nd == NULL)
			return false;
		t->defs = nd;
		t->alloc = na;
	}
	t->defs[t->n++] = *d;
	return true;
}

static void g_skip_names(const char **p)
{
	g_ws(p);
	while (**p == '[') {
		while (**p != '\0' && **p != ']')
			(*p)++;
		if (**p == ']')
			(*p)++;
		g_ws(p);
	}
}

/* Parse a <track-list>; returns false for none / unparsable */
static bool g_parse_template(const char *s, struct gtemplate *t)
{
	const char *p = s;
	memset(t, 0, sizeof(*t));
	if (s == NULL)
		return false;
	for (;;) {
		struct gdef d;
		g_skip_names(&p);
		if (*p == '\0')
			break;
		if (*p == '"') {
			/* area strings inside grid-template: skip */
			p++;
			while (*p != '\0' && *p != '"')
				p++;
			if (*p == '"')
				p++;
			continue;
		}
		if (g_word(p, "repeat(")) {
			int count = 1, first = t->n;
			bool autorep = false, fit = false;
			p += 7;
			g_ws(&p);
			if (g_word(p, "auto-fill")) {
				autorep = true;
				p += 9;
			} else if (g_word(p, "auto-fit")) {
				autorep = fit = true;
				p += 8;
			} else {
				count = strtol(p, (char **) &p, 10);
				if (count < 1)
					count = 1;
			}
			g_ws(&p);
			if (*p == ',')
				p++;
			for (;;) {
				g_skip_names(&p);
				if (*p == '\0')
					break;
				if (*p == ')') {
					p++;
					break;
				}
				if (!g_parse_track(&p, &d))
					goto fail;
				if (!g_tpl_add(t, &d))
					break;
			}
			if (autorep) {
				if (t->rep_len == 0 && t->n > first) {
					t->rep_start = first;
					t->rep_len = t->n - first;
					t->rep_fit = fit;
				}
			} else {
				int len = t->n - first;
				for (int r = 1; r < count && len > 0; r++)
					for (int i = 0; i < len; i++)
						if (!g_tpl_add(t, &t->defs[first + i]))
							break;
			}
			continue;
		}
		if (*p == ')' || *p == ',') {
			p++;
			continue;
		}
		if (!g_parse_track(&p, &d))
			goto fail;
		if (g_word(s, "none") && t->n == 0 && d.min.type == GS_AUTO)
			break;
		if (!g_tpl_add(t, &d))
			break;
	}
	return t->n > 0;
fail:
	return t->n > 0;
}

/* ------------------------------------------------------------------ */
/* lengths */

static int g_len(const struct grid *g, const struct gsize *s, int pct_base)
{
	switch (s->type) {
	case GS_LEN:
		return FIXTOINT(css_unit_len2device_px(g->box->style, g->uctx,
				FLTTOFIX(s->v), s->unit));
	case GS_PCT:
		if (pct_base < 0)
			return G_INF;
		return (int) (s->v * pct_base / 100.0f);
	default:
		return G_INF;
	}
}

static bool g_definite(const struct gsize *s, int pct_base)
{
	return s->type == GS_LEN || (s->type == GS_PCT && pct_base >= 0);
}

int layout_grid_gap(const css_unit_ctx *uctx, const css_computed_style *style,
		bool column, int pct_base)
{
	css_fixed len = 0;
	css_unit unit = CSS_UNIT_PX;
	uint8_t type;

	if (style == NULL)
		return 0;
	if (column)
		type = css_computed_column_gap(style, &len, &unit);
	else
		type = css_computed_row_gap(style, &len, &unit);
	if (type != CSS_COLUMN_GAP_SET)
		return 0;
	if (unit == CSS_UNIT_PCT) {
		if (pct_base <= 0)
			return 0;
		return FPCT_OF_INT_TOINT(len, pct_base);
	}
	return FIXTOINT(css_unit_len2device_px(style, uctx, len, unit));
}

/* ------------------------------------------------------------------ */
/* areas and placement */

static void g_parse_areas(struct grid *g, const char *s)
{
	int row = 0;
	g->nareas = 0;
	g->area_rows = g->area_cols = 0;
	if (s == NULL)
		return;
	while (*s != '\0') {
		int col = 0;
		if (*s != '"') {
			s++;
			continue;
		}
		s++;
		while (*s != '\0' && *s != '"') {
			char name[48];
			size_t n = 0;
			while (*s == ' ')
				s++;
			if (*s == '"' || *s == '\0')
				break;
			while (*s != '\0' && *s != '"' && *s != ' ') {
				if (n < sizeof(name) - 1)
					name[n++] = *s;
				s++;
			}
			name[n] = '\0';
			if (name[0] != '.') {
				int a;
				for (a = 0; a < g->nareas; a++)
					if (strcmp(g->areas[a].name, name) == 0)
						break;
				if (a == g->nareas && a < 32) {
					strcpy(g->areas[a].name, name);
					g->areas[a].r0 = row;
					g->areas[a].c0 = col;
					g->areas[a].r1 = row + 1;
					g->areas[a].c1 = col + 1;
					g->nareas++;
				} else if (a < g->nareas) {
					struct garea *ar = &g->areas[a];
					if (row < ar->r0) ar->r0 = row;
					if (col < ar->c0) ar->c0 = col;
					if (row + 1 > ar->r1) ar->r1 = row + 1;
					if (col + 1 > ar->c1) ar->c1 = col + 1;
				}
			}
			col++;
		}
		if (*s == '"')
			s++;
		if (col > g->area_cols)
			g->area_cols = col;
		row++;
	}
	g->area_rows = row;
}

static void g_parse_line(lwc_string *ls, struct gline *l)
{
	char buf[128];
	const char *p = g_str(ls, buf, sizeof buf);
	bool span = false, have_n = false;
	int n = 0;

	l->type = GL_AUTO;
	l->n = 0;
	l->name[0] = '\0';
	if (p == NULL)
		return;
	while (*p != '\0') {
		g_ws(&p);
		if (*p == '\0')
			break;
		if (g_word(p, "span") && !g_identc(p[4])) {
			span = true;
			p += 4;
		} else if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+') {
			char *end;
			long v = strtol(p, &end, 10);
			if (end == p) {
				p++;
				continue;
			}
			n = (int) v;
			have_n = true;
			p = end;
		} else if (g_identc(*p)) {
			size_t k = 0;
			const char *q = p;
			while (g_identc(*p))
				p++;
			if (!(p - q == 4 && strncasecmp(q, "auto", 4) == 0)) {
				k = p - q;
				if (k >= sizeof(l->name))
					k = sizeof(l->name) - 1;
				memcpy(l->name, q, k);
				l->name[k] = '\0';
			}
		} else {
			p++;
		}
	}
	if (span) {
		l->type = GL_SPAN;
		l->n = (have_n && n > 0) ? n : 1;
	} else if (have_n && n != 0) {
		l->type = GL_LINE;
		l->n = n;
	} else if (l->name[0] != '\0') {
		l->type = GL_NAME;
	}
}

/* 0-based line index for a named line, or -1 */
static int g_name_line(const struct grid *g, const char *name, bool row,
		bool start)
{
	char base[48];
	size_t l = strlen(name);
	strcpy(base, name);
	if (l > 6 && strcmp(name + l - 6, "-start") == 0) {
		base[l - 6] = '\0';
		start = true;
	} else if (l > 4 && strcmp(name + l - 4, "-end") == 0) {
		base[l - 4] = '\0';
		start = false;
	}
	for (int a = 0; a < g->nareas; a++) {
		const struct garea *ar = &g->areas[a];
		if (strcmp(ar->name, base) != 0)
			continue;
		if (row)
			return start ? ar->r0 : ar->r1;
		return start ? ar->c0 : ar->c1;
	}
	return -1;
}

static int g_line_index(const struct grid *g, const struct gline *l,
		int nexplicit, bool row, bool start)
{
	int i;
	if (l->type == GL_LINE) {
		i = (l->n > 0) ? l->n - 1 : nexplicit + 1 + l->n;
		return (i < 0) ? 0 : i;
	}
	if (l->type == GL_NAME)
		return g_name_line(g, l->name, row, start);
	return -1;
}

static void g_resolve_axis(const struct grid *g, const struct gline *s,
		const struct gline *e, int nexplicit, bool row,
		int *pos, int *span, bool *def)
{
	int a = g_line_index(g, s, nexplicit, row, true);
	int b = g_line_index(g, e, nexplicit, row, false);

	*def = true;
	if (a >= 0 && b >= 0) {
		if (b < a) {
			int t = a;
			a = b;
			b = t;
		}
		if (a == b)
			b = a + 1;
		*pos = a;
		*span = b - a;
	} else if (a >= 0) {
		*pos = a;
		*span = (e->type == GL_SPAN) ? e->n : 1;
	} else if (b >= 0) {
		*span = (s->type == GL_SPAN) ? s->n : 1;
		*pos = b - *span;
		if (*pos < 0) {
			*span += *pos;
			*pos = 0;
			if (*span < 1)
				*span = 1;
		}
	} else {
		*def = false;
		*pos = 0;
		*span = (s->type == GL_SPAN) ? s->n :
				((e->type == GL_SPAN) ? e->n : 1);
	}
	if (*span > G_MAX_TRACKS)
		*span = G_MAX_TRACKS;
	if (*pos > G_MAX_TRACKS)
		*pos = G_MAX_TRACKS;
}

struct gocc {
	unsigned char *cells;
	int major;   /* allocated major lines */
	int minor;
};

static bool g_occ_ensure(struct gocc *o, int major)
{
	unsigned char *n;
	int nm;
	if (major <= o->major)
		return true;
	nm = o->major ? o->major : 8;
	while (nm < major)
		nm *= 2;
	n = realloc(o->cells, (size_t) nm * o->minor);
	if (n == NULL)
		return false;
	memset(n + (size_t) o->major * o->minor, 0,
			(size_t) (nm - o->major) * o->minor);
	o->cells = n;
	o->major = nm;
	return true;
}

static bool g_occ_fits(struct gocc *o, int ma, int mi, int maspan, int mispan)
{
	if (mi + mispan > o->minor)
		return false;
	if (!g_occ_ensure(o, ma + maspan))
		return false;
	for (int a = ma; a < ma + maspan; a++)
		for (int b = mi; b < mi + mispan; b++)
			if (o->cells[(size_t) a * o->minor + b])
				return false;
	return true;
}

static void g_occ_mark(struct gocc *o, int ma, int mi, int maspan, int mispan)
{
	if (!g_occ_ensure(o, ma + maspan))
		return;
	for (int a = ma; a < ma + maspan; a++)
		for (int b = mi; b < mi + mispan && b < o->minor; b++)
			o->cells[(size_t) a * o->minor + b] = 1;
}

/* auto placement; returns number of major tracks used */
static bool g_place(struct grid *g, int nexp_cols, int nexp_rows,
		int *ncols, int *nrows)
{
	struct gocc o = { NULL, 0, 0 };
	bool cf = g->column_flow;
	int minor = cf ? nexp_rows : nexp_cols;
	int major_used = cf ? nexp_cols : nexp_rows;
	int cur_ma = 0, cur_mi = 0;

#define MA_POS(it) (cf ? (it)->c0 : (it)->r0)
#define MA_SPAN(it) (cf ? (it)->cspan : (it)->rspan)
#define MA_DEF(it) (cf ? (it)->cdef : (it)->rdef)
#define MI_POS(it) (cf ? (it)->r0 : (it)->c0)
#define MI_SPAN(it) (cf ? (it)->rspan : (it)->cspan)
#define MI_DEF(it) (cf ? (it)->rdef : (it)->cdef)

	for (int i = 0; i < g->nitems; i++) {
		struct gitem *it = &g->items[i];
		if (MI_DEF(it) && MI_POS(it) + MI_SPAN(it) > minor)
			minor = MI_POS(it) + MI_SPAN(it);
		if (MI_SPAN(it) > minor)
			minor = MI_SPAN(it);
	}
	if (minor < 1)
		minor = 1;
	if (minor > G_MAX_TRACKS)
		minor = G_MAX_TRACKS;
	o.minor = minor;

	/* 1: fully definite; 2: definite major position */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < g->nitems; i++) {
			struct gitem *it = &g->items[i];
			int *mip = cf ? &it->r0 : &it->c0;
			if (!MA_DEF(it))
				continue;
			if (pass == 0 && !MI_DEF(it))
				continue;
			if (pass == 1 && MI_DEF(it))
				continue;
			if (pass == 1) {
				int m;
				for (m = 0; m + MI_SPAN(it) <= minor; m++)
					if (g_occ_fits(&o, MA_POS(it), m,
							MA_SPAN(it), MI_SPAN(it)))
						break;
				if (m + MI_SPAN(it) > minor)
					m = 0;
				*mip = m;
				if (cf)
					it->rdef = true;
				else
					it->cdef = true;
			}
			g_occ_mark(&o, MA_POS(it), MI_POS(it), MA_SPAN(it),
					MI_SPAN(it));
		}
	}

	/* 3: the rest */
	for (int i = 0; i < g->nitems; i++) {
		struct gitem *it = &g->items[i];
		int *map = cf ? &it->c0 : &it->r0;
		int *mip = cf ? &it->r0 : &it->c0;
		int guard = 0;
		if (MA_DEF(it))
			continue;
		if (g->dense) {
			cur_ma = 0;
			cur_mi = 0;
		}
		if (MI_DEF(it)) {
			if (MI_POS(it) < cur_mi)
				cur_ma++;
			while (!g_occ_fits(&o, cur_ma, MI_POS(it), MA_SPAN(it),
					MI_SPAN(it)) && guard++ < 100000)
				cur_ma++;
			*map = cur_ma;
			cur_mi = MI_POS(it) + MI_SPAN(it);
		} else {
			for (;;) {
				if (guard++ > 1000000)
					break;
				if (cur_mi + MI_SPAN(it) > minor) {
					cur_ma++;
					cur_mi = 0;
					continue;
				}
				if (g_occ_fits(&o, cur_ma, cur_mi, MA_SPAN(it),
						MI_SPAN(it)))
					break;
				cur_mi++;
			}
			*map = cur_ma;
			*mip = cur_mi;
			cur_mi += MI_SPAN(it);
		}
		if (cf) {
			it->cdef = it->rdef = true;
		} else {
			it->cdef = it->rdef = true;
		}
		g_occ_mark(&o, MA_POS(it), MI_POS(it), MA_SPAN(it), MI_SPAN(it));
	}

	for (int i = 0; i < g->nitems; i++) {
		struct gitem *it = &g->items[i];
		if (MA_POS(it) + MA_SPAN(it) > major_used)
			major_used = MA_POS(it) + MA_SPAN(it);
	}
	if (major_used > G_MAX_TRACKS)
		major_used = G_MAX_TRACKS;
	free(o.cells);

	*ncols = cf ? major_used : minor;
	*nrows = cf ? minor : major_used;
#undef MA_POS
#undef MA_SPAN
#undef MA_DEF
#undef MI_POS
#undef MI_SPAN
#undef MI_DEF
	return true;
}

/* ------------------------------------------------------------------ */
/* track sizing */

static bool g_is_flex(const struct gtrack *t)
{
	return t->def.max.type == GS_FR && !t->collapsed;
}

static bool g_min_intrinsic(const struct gtrack *t, int pct)
{
	return !g_definite(&t->def.min, pct);
}

static bool g_max_intrinsic(const struct gtrack *t, int pct)
{
	return t->def.max.type != GS_FR && !g_definite(&t->def.max, pct);
}

static int g_visible(const struct gtrack *t, int n)
{
	int v = 0;
	for (int i = 0; i < n; i++)
		if (!t[i].collapsed)
			v++;
	return v;
}

/*
 * Size the tracks of one axis.
 * avail < 0: indefinite (max-content sizing).  minmode: min-content sizing.
 */
static void g_size_tracks(struct grid *g, struct gtrack *t, int n, int avail,
		int gap, bool cols, bool stretch, bool minmode)
{
	int pct = avail;
	int vis = g_visible(t, n);
	int gaps = (vis > 1) ? (vis - 1) * gap : 0;
	bool any_flex = false;

	/* initialise */
	for (int i = 0; i < n; i++) {
		struct gtrack *tr = &t[i];
		if (tr->collapsed) {
			tr->base = tr->limit = tr->size = 0;
			continue;
		}
		tr->base = g_definite(&tr->def.min, pct) ?
				g_len(g, &tr->def.min, pct) : 0;
		if (tr->def.max.type == GS_FR) {
			tr->limit = tr->base;
			any_flex = true;
		} else if (g_definite(&tr->def.max, pct)) {
			tr->limit = g_len(g, &tr->def.max, pct);
		} else {
			tr->limit = G_INF;
		}
		if (tr->limit != G_INF && tr->limit < tr->base)
			tr->limit = tr->base;
	}

	/* intrinsic contributions: single-track items first, then spans */
	for (int pass = 0; pass < 2; pass++) {
		for (int k = 0; k < g->nitems; k++) {
			const struct gitem *it = &g->items[k];
			int p0 = cols ? it->c0 : it->r0;
			int sp = cols ? it->cspan : it->rspan;
			int minc = cols ? it->minc : it->h;
			int maxc = cols ? it->maxc : it->h;
			bool crosses_flex = false;
			int sum_base = 0, sum_lim = 0, nmin = 0, nmax = 0, nflex = 0;
			if (minmode)
				maxc = minc;
			if ((sp == 1) != (pass == 0))
				continue;
			if (p0 + sp > n)
				continue;
			for (int i = p0; i < p0 + sp; i++) {
				if (t[i].collapsed)
					continue;
				if (g_is_flex(&t[i])) {
					crosses_flex = true;
					if (g_min_intrinsic(&t[i], pct))
						nflex++;
				}
				sum_base += t[i].base;
				sum_lim += (t[i].limit == G_INF) ?
						t[i].base : t[i].limit;
				if (g_min_intrinsic(&t[i], pct))
					nmin++;
				if (g_max_intrinsic(&t[i], pct))
					nmax++;
			}
			sum_base += (sp - 1) * gap;
			sum_lim += (sp - 1) * gap;
			if (crosses_flex) {
				/* grow flexible tracks with auto minimum */
				int need = minc - sum_base;
				if (need > 0 && nflex > 0) {
					for (int i = p0; i < p0 + sp; i++) {
						if (!g_is_flex(&t[i]) ||
						    !g_min_intrinsic(&t[i], pct))
							continue;
						t[i].base += need / nflex;
					}
				}
				continue;
			}
			if (sp == 1) {
				struct gtrack *tr = &t[p0];
				if (tr->collapsed)
					continue;
				if (g_min_intrinsic(tr, pct)) {
					int c = (tr->def.min.type == GS_MAXC) ?
							maxc : minc;
					if (c > tr->base)
						tr->base = c;
				}
				if (g_max_intrinsic(tr, pct)) {
					int c = (tr->def.max.type == GS_MINC) ?
							minc : maxc;
					if (tr->limit == G_INF || c > tr->limit)
						tr->limit = c;
				}
				continue;
			}
			if (minc > sum_base && nmin > 0) {
				int need = minc - sum_base;
				for (int i = p0; i < p0 + sp; i++)
					if (!t[i].collapsed &&
					    g_min_intrinsic(&t[i], pct))
						t[i].base += need / nmin;
			}
			if (maxc > sum_lim && nmax > 0) {
				int need = maxc - sum_lim;
				for (int i = p0; i < p0 + sp; i++) {
					if (t[i].collapsed ||
					    !g_max_intrinsic(&t[i], pct))
						continue;
					if (t[i].limit == G_INF)
						t[i].limit = t[i].base;
					t[i].limit += need / nmax;
				}
			}
		}
	}
	for (int i = 0; i < n; i++) {
		if (t[i].limit == G_INF || t[i].limit < t[i].base)
			t[i].limit = t[i].base;
		t[i].size = t[i].base;
	}

	/* maximise tracks */
	if (avail >= 0) {
		int guard = 0;
		for (;;) {
			int used = gaps, count = 0, free_space;
			for (int i = 0; i < n; i++) {
				used += t[i].size;
				if (!t[i].collapsed && !g_is_flex(&t[i]) &&
				    t[i].size < t[i].limit)
					count++;
			}
			free_space = avail - used;
			if (free_space <= 0 || count == 0 || guard++ > 64)
				break;
			for (int i = 0; i < n; i++) {
				int share;
				if (t[i].collapsed || g_is_flex(&t[i]) ||
				    t[i].size >= t[i].limit)
					continue;
				share = free_space / count;
				if (share < 1)
					share = 1;
				if (share > t[i].limit - t[i].size)
					share = t[i].limit - t[i].size;
				t[i].size += share;
			}
		}
	} else if (!minmode) {
		for (int i = 0; i < n; i++)
			if (!g_is_flex(&t[i]))
				t[i].size = t[i].limit;
	}

	/* flexible tracks */
	if (any_flex) {
		float fr = 0;
		if (avail >= 0) {
			bool *frozen = calloc(n, sizeof(bool));
			int guard = 0;
			bool changed = true;
			while (changed && guard++ < 32) {
				float sumf = 0;
				int space = avail - gaps;
				changed = false;
				for (int i = 0; i < n; i++) {
					if (!g_is_flex(&t[i]) ||
					    (frozen && frozen[i]))
						space -= t[i].size;
					else
						sumf += t[i].def.max.v;
				}
				if (sumf < 1)
					sumf = 1;
				fr = (space > 0) ? space / sumf : 0;
				for (int i = 0; i < n; i++) {
					if (!g_is_flex(&t[i]) ||
					    (frozen && frozen[i]))
						continue;
					if (fr * t[i].def.max.v < t[i].base &&
					    frozen) {
						frozen[i] = true;
						changed = true;
					}
				}
			}
			for (int i = 0; i < n; i++) {
				int s;
				if (!g_is_flex(&t[i]) || (frozen && frozen[i]))
					continue;
				s = (int) (fr * t[i].def.max.v);
				t[i].size = (s > t[i].base) ? s : t[i].base;
			}
			free(frozen);
		} else {
			for (int i = 0; i < n; i++) {
				float f;
				if (!g_is_flex(&t[i]))
					continue;
				f = t[i].def.max.v;
				f = (f > 1) ? t[i].base / f : t[i].base;
				if (f > fr)
					fr = f;
			}
			for (int k = 0; k < g->nitems && !minmode; k++) {
				const struct gitem *it = &g->items[k];
				int p0 = cols ? it->c0 : it->r0;
				int sp = cols ? it->cspan : it->rspan;
				int c = cols ? it->maxc : it->h;
				float sumf = 0;
				if (p0 + sp > n)
					continue;
				for (int i = p0; i < p0 + sp; i++) {
					if (g_is_flex(&t[i]))
						sumf += t[i].def.max.v;
					else
						c -= t[i].size;
				}
				c -= (sp - 1) * gap;
				if (sumf <= 0)
					continue;
				if (sumf < 1)
					sumf = 1;
				if (c / sumf > fr)
					fr = c / sumf;
			}
			for (int i = 0; i < n; i++) {
				int s;
				if (!g_is_flex(&t[i]))
					continue;
				s = (int) (fr * t[i].def.max.v + 0.5f);
				t[i].size = (s > t[i].base) ? s : t[i].base;
			}
		}
	}

	/* stretch auto tracks */
	if (stretch && avail >= 0) {
		int used = gaps, count = 0, free_space;
		for (int i = 0; i < n; i++) {
			used += t[i].size;
			if (!t[i].collapsed && t[i].def.max.type == GS_AUTO)
				count++;
		}
		free_space = avail - used;
		if (free_space > 0 && count > 0) {
			int share = free_space / count;
			int rem = free_space - share * count;
			for (int i = 0; i < n; i++) {
				if (t[i].collapsed ||
				    t[i].def.max.type != GS_AUTO)
					continue;
				t[i].size += share + (rem > 0 ? 1 : 0);
				if (rem > 0)
					rem--;
			}
		}
	}
}

/* position tracks; returns total extent */
static int g_position_tracks(struct gtrack *t, int n, int gap, int origin,
		int avail, int distribution)
{
	int total = 0, vis = g_visible(t, n);
	int lead = 0, between = 0, free_space;
	int cur;
	bool first = true;

	for (int i = 0; i < n; i++)
		total += t[i].size;
	if (vis > 1)
		total += (vis - 1) * gap;

	free_space = (avail >= 0) ? avail - total : 0;
	if (free_space > 0 && vis > 0) {
		switch (distribution) {
		case CSS_JUSTIFY_CONTENT_FLEX_END:
			lead = free_space;
			break;
		case CSS_JUSTIFY_CONTENT_CENTER:
			lead = free_space / 2;
			break;
		case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN:
			between = (vis > 1) ? free_space / (vis - 1) : 0;
			break;
		case CSS_JUSTIFY_CONTENT_SPACE_AROUND:
			between = free_space / vis;
			lead = between / 2;
			break;
		case CSS_JUSTIFY_CONTENT_SPACE_EVENLY:
			between = free_space / (vis + 1);
			lead = between;
			break;
		default:
			break;
		}
	}

	cur = origin + lead;
	for (int i = 0; i < n; i++) {
		if (t[i].collapsed) {
			t[i].pos = cur;
			continue;
		}
		if (!first)
			cur += gap + between;
		first = false;
		t[i].pos = cur;
		cur += t[i].size;
	}
	return total;
}

static int g_area(const struct gtrack *t, int n, int p0, int sp)
{
	int end;
	if (p0 >= n)
		return 0;
	end = p0 + sp;
	if (end > n)
		end = n;
	return t[end - 1].pos + t[end - 1].size - t[p0].pos;
}

/* ------------------------------------------------------------------ */
/* container setup */

enum galign { GA_STRETCH, GA_START, GA_END, GA_CENTER };

static enum galign g_justify_of(const struct box *grid, const struct box *b)
{
	char buf[64];
	const char *s = g_str(NULL, buf, sizeof buf);
	lwc_string *ls = NULL;

	css_computed_justify_self(b->style, &ls);
	s = g_str(ls, buf, sizeof buf);
	if (s == NULL || strcasecmp(s, "auto") == 0) {
		ls = NULL;
		css_computed_justify_items(grid->style, &ls);
		s = g_str(ls, buf, sizeof buf);
	}
	if (s == NULL)
		return GA_STRETCH;
	if (strstr(s, "center"))
		return GA_CENTER;
	if (strstr(s, "end") || strstr(s, "right"))
		return GA_END;
	if (strstr(s, "start") || strstr(s, "left") || strstr(s, "baseline"))
		return GA_START;
	return GA_STRETCH;
}

static enum galign g_align_of(const struct box *grid, const struct box *b)
{
	switch (lh__box_align_self(grid, b)) {
	case CSS_ALIGN_SELF_FLEX_START:
	case CSS_ALIGN_SELF_BASELINE:
		return GA_START;
	case CSS_ALIGN_SELF_FLEX_END:
		return GA_END;
	case CSS_ALIGN_SELF_CENTER:
		return GA_CENTER;
	default:
		return GA_STRETCH;
	}
}

static void g_fill_tracks(struct grid *g, struct gtrack *t, int n,
		const struct gtemplate *tpl, const struct gtemplate *autot,
		int nexp)
{
	for (int i = 0; i < n; i++) {
		memset(&t[i], 0, sizeof(t[i]));
		if (i < nexp && i < tpl->n) {
			t[i].def = tpl->defs[i];
		} else if (autot->n > 0) {
			int k = (i - nexp) % autot->n;
			if (k < 0)
				k = 0;
			t[i].def = autot->defs[k];
		} else {
			t[i].def.min.type = GS_AUTO;
			t[i].def.max.type = GS_AUTO;
		}
	}
	(void) g;
}

/* Expand auto-repeat into tpl given the available size */
static void g_expand_repeat(const struct grid *g, struct gtemplate *tpl,
		int avail, int gap, bool *fit, int *rep_first, int *rep_count)
{
	int k = 1, fixed = 0, rep = 0, nfixed;
	struct gdef *nd;
	int nn;

	*fit = false;
	*rep_first = 0;
	*rep_count = 0;
	if (tpl->rep_len == 0)
		return;
	nfixed = tpl->n - tpl->rep_len;
	for (int i = 0; i < tpl->n; i++) {
		const struct gdef *d = &tpl->defs[i];
		int s = 0;
		if (g_definite(&d->max, avail))
			s = g_len(g, &d->max, avail);
		else if (g_definite(&d->min, avail))
			s = g_len(g, &d->min, avail);
		if (s < 0)
			s = 0;
		if (i >= tpl->rep_start && i < tpl->rep_start + tpl->rep_len)
			rep += s;
		else
			fixed += s;
	}
	if (avail >= 0 && rep > 0) {
		int space = avail - fixed - nfixed * gap + gap;
		k = space / (rep + tpl->rep_len * gap);
		if (k < 1)
			k = 1;
	}
	if (k * tpl->rep_len + nfixed > G_MAX_TRACKS)
		k = (G_MAX_TRACKS - nfixed) / tpl->rep_len;
	if (k < 1)
		k = 1;
	nn = nfixed + k * tpl->rep_len;
	nd = malloc(nn * sizeof(*nd));
	if (nd == NULL)
		return;
	memcpy(nd, tpl->defs, tpl->rep_start * sizeof(*nd));
	for (int r = 0; r < k; r++)
		memcpy(nd + tpl->rep_start + r * tpl->rep_len,
				tpl->defs + tpl->rep_start,
				tpl->rep_len * sizeof(*nd));
	memcpy(nd + tpl->rep_start + k * tpl->rep_len,
			tpl->defs + tpl->rep_start + tpl->rep_len,
			(tpl->n - tpl->rep_start - tpl->rep_len) * sizeof(*nd));
	*fit = tpl->rep_fit;
	*rep_first = tpl->rep_start;
	*rep_count = k * tpl->rep_len;
	free(tpl->defs);
	tpl->defs = nd;
	tpl->n = tpl->alloc = nn;
	tpl->rep_len = 0;
}

static void g_destroy(struct grid *g)
{
	free(g->items);
	free(g->cols);
	free(g->rows);
}

static bool g_setup(struct grid *g, struct box *box, html_content *content,
		int avail_w, int avail_h)
{
	char buf[2048];
	struct gtemplate tc, tr, ac, ar;
	lwc_string *ls;
	int nexp_cols, nexp_rows, i;
	bool cfit, rfit;
	int cfirst, ccount, rfirst, rcount;
	const char *flow;

	memset(g, 0, sizeof(*g));
	g->box = box;
	g->content = content;
	g->uctx = &content->unit_len_ctx;
	g->col_gap = layout_grid_gap(g->uctx, box->style, true, avail_w);
	g->row_gap = layout_grid_gap(g->uctx, box->style, false, avail_h);

	ls = NULL;
	css_computed_grid_auto_flow(box->style, &ls);
	flow = g_str(ls, buf, sizeof buf);
	if (flow != NULL) {
		g->column_flow = (strstr(flow, "column") != NULL);
		g->dense = (strstr(flow, "dense") != NULL);
	}

	ls = NULL;
	css_computed_grid_template_areas(box->style, &ls);
	g_parse_areas(g, g_str(ls, buf, sizeof buf));

	ls = NULL;
	css_computed_grid_template_columns(box->style, &ls);
	g_parse_template(g_str(ls, buf, sizeof buf), &tc);
	ls = NULL;
	css_computed_grid_template_rows(box->style, &ls);
	g_parse_template(g_str(ls, buf, sizeof buf), &tr);
	ls = NULL;
	css_computed_grid_auto_columns(box->style, &ls);
	g_parse_template(g_str(ls, buf, sizeof buf), &ac);
	ls = NULL;
	css_computed_grid_auto_rows(box->style, &ls);
	g_parse_template(g_str(ls, buf, sizeof buf), &ar);

	g_expand_repeat(g, &tc, avail_w, g->col_gap, &cfit, &cfirst, &ccount);
	g_expand_repeat(g, &tr, avail_h, g->row_gap, &rfit, &rfirst, &rcount);

	nexp_cols = tc.n > g->area_cols ? tc.n : g->area_cols;
	nexp_rows = tr.n > g->area_rows ? tr.n : g->area_rows;

	/* items */
	g->nitems = 0;
	for (struct box *b = box->children; b != NULL; b = b->next)
		g->nitems++;
	g->items = calloc(g->nitems ? g->nitems : 1, sizeof(*g->items));
	if (g->items == NULL)
		goto fail;
	i = 0;
	for (struct box *b = box->children; b != NULL; b = b->next) {
		struct gitem *it = &g->items[i];
		struct gline cs, ce, rs, re;
		it->b = b;
		ls = NULL; css_computed_grid_column_start(b->style, &ls);
		g_parse_line(ls, &cs);
		ls = NULL; css_computed_grid_column_end(b->style, &ls);
		g_parse_line(ls, &ce);
		ls = NULL; css_computed_grid_row_start(b->style, &ls);
		g_parse_line(ls, &rs);
		ls = NULL; css_computed_grid_row_end(b->style, &ls);
		g_parse_line(ls, &re);
		if (lh__box_is_absolute(b)) {
			cs.type = ce.type = rs.type = re.type = GL_AUTO;
		}
		g_resolve_axis(g, &cs, &ce, nexp_cols, false,
				&it->c0, &it->cspan, &it->cdef);
		g_resolve_axis(g, &rs, &re, nexp_rows, true,
				&it->r0, &it->rspan, &it->rdef);
		it->minc = (b->min_width > 0) ? b->min_width : 0;
		it->maxc = (b->max_width > it->minc &&
				b->max_width != UNKNOWN_MAX_WIDTH) ?
				b->max_width : it->minc;
		i++;
	}

	/* absolutely positioned children take no part in placement */
	{
		int k = 0;
		for (i = 0; i < g->nitems; i++)
			if (!lh__box_is_absolute(g->items[i].b))
				g->items[k++] = g->items[i];
		g->nitems = k;
	}

	g_place(g, nexp_cols, nexp_rows, &g->ncols, &g->nrows);
	if (g->ncols < nexp_cols)
		g->ncols = nexp_cols;
	if (g->nrows < nexp_rows)
		g->nrows = nexp_rows;
	if (g->ncols < 1)
		g->ncols = 1;

	g->cols = calloc(g->ncols, sizeof(*g->cols));
	g->rows = calloc(g->nrows ? g->nrows : 1, sizeof(*g->rows));
	if (g->cols == NULL || g->rows == NULL)
		goto fail;
	g_fill_tracks(g, g->cols, g->ncols, &tc, &ac, tc.n);
	g_fill_tracks(g, g->rows, g->nrows, &tr, &ar, tr.n);

	/* auto-fit: collapse empty repeated tracks */
	for (i = 0; i < g->nitems; i++) {
		struct gitem *it = &g->items[i];
		for (int c = it->c0; c < it->c0 + it->cspan && c < g->ncols; c++)
			g->cols[c].used = true;
		for (int r = it->r0; r < it->r0 + it->rspan && r < g->nrows; r++)
			g->rows[r].used = true;
	}
	if (cfit)
		for (i = cfirst; i < cfirst + ccount && i < g->ncols; i++)
			g->cols[i].collapsed = !g->cols[i].used;
	if (rfit)
		for (i = rfirst; i < rfirst + rcount && i < g->nrows; i++)
			g->rows[i].collapsed = !g->rows[i].used;

	free(tc.defs); free(tr.defs); free(ac.defs); free(ar.defs);
	return true;
fail:
	free(tc.defs); free(tr.defs); free(ac.defs); free(ar.defs);
	g_destroy(g);
	return false;
}

/* ------------------------------------------------------------------ */

static bool g_layout_item(struct box *b, int available_width,
		html_content *content)
{
	bool ok;
	switch (b->type) {
	case BOX_BLOCK:
		ok = layout_block_context(b, -1, content);
		break;
	case BOX_TABLE:
		b->float_container = b->parent;
		ok = layout_table(b, available_width, content);
		b->float_container = NULL;
		break;
	case BOX_FLEX:
		b->float_container = b->parent;
		ok = layout_flex(b, available_width, content);
		b->float_container = NULL;
		break;
	default:
		ok = true;
		break;
	}
	return ok;
}

bool layout_grid(struct box *grid, int available_width,
		html_content *content)
{
	struct grid g;
	int max_height, min_height;
	int height_avail;
	int total_h;
	bool ok = true;

	layout_find_dimensions(&content->unit_len_ctx, available_width, -1,
			grid, grid->style, NULL, &grid->height,
			NULL, NULL, &max_height, &min_height,
			grid->margin, grid->padding, grid->border);

	available_width = min(available_width, grid->width);
	if (available_width < 0)
		available_width = 0;
	height_avail = (grid->height != AUTO) ? grid->height : -1;

	if (!g_setup(&g, grid, content, available_width, height_avail))
		return false;

	g_size_tracks(&g, g.cols, g.ncols, available_width, g.col_gap, true,
			css_computed_justify_content(grid->style) ==
				CSS_JUSTIFY_CONTENT_FLEX_START, false);
	g_position_tracks(g.cols, g.ncols, g.col_gap, grid->padding[LEFT],
			available_width,
			css_computed_justify_content(grid->style));

	/* lay out items at their column widths */
	for (int i = 0; i < g.nitems; i++) {
		struct gitem *it = &g.items[i];
		struct box *b = it->b;
		int area_w = g_area(g.cols, g.ncols, it->c0, it->cspan);
		int max_w, min_w, max_h, min_h;
		enum galign j = g_justify_of(grid, b);

		b->float_container = b->parent;
		layout_find_dimensions(&content->unit_len_ctx, area_w, -1,
				b, b->style, &b->width, &b->height,
				&max_w, &min_w, &max_h, &min_h,
				b->margin, b->padding, b->border);
		b->float_container = NULL;

		if (b->width == AUTO) {
			int w;
			if (j == GA_STRETCH && !lh__box_is_replace(b)) {
				w = area_w;
			} else {
				w = min(max(b->min_width, 0), area_w);
				w = max(w, min(b->max_width, area_w));
			}
			b->width = w - lh__delta_outer_width(b);
		}
		if (max_w >= 0 && b->width > max_w)
			b->width = max_w;
		if (min_w > 0 && b->width < min_w)
			b->width = min_w;
		if (b->width < 0)
			b->width = 0;

		if (!g_layout_item(b, area_w, content)) {
			ok = false;
			break;
		}
		if (b->height == AUTO)
			b->height = 0;
		if (max_h >= 0 && b->height > max_h)
			b->height = max_h;
		if (min_h > 0 && b->height < min_h)
			b->height = min_h;
		it->h = b->height + lh__delta_outer_height(b);
	}

	if (ok) {
		g_size_tracks(&g, g.rows, g.nrows, height_avail, g.row_gap,
				false, css_computed_align_content(grid->style) ==
					CSS_ALIGN_CONTENT_STRETCH, false);
		/* align-content values map onto the justify-content ones */
		{
			int ac = CSS_JUSTIFY_CONTENT_FLEX_START;
			switch (css_computed_align_content(grid->style)) {
			case CSS_ALIGN_CONTENT_FLEX_END:
				ac = CSS_JUSTIFY_CONTENT_FLEX_END; break;
			case CSS_ALIGN_CONTENT_CENTER:
				ac = CSS_JUSTIFY_CONTENT_CENTER; break;
			case CSS_ALIGN_CONTENT_SPACE_BETWEEN:
				ac = CSS_JUSTIFY_CONTENT_SPACE_BETWEEN; break;
			case CSS_ALIGN_CONTENT_SPACE_AROUND:
				ac = CSS_JUSTIFY_CONTENT_SPACE_AROUND; break;
			case CSS_ALIGN_CONTENT_SPACE_EVENLY:
				ac = CSS_JUSTIFY_CONTENT_SPACE_EVENLY; break;
			default:
				break;
			}
			total_h = g_position_tracks(g.rows, g.nrows, g.row_gap,
					grid->padding[TOP], height_avail, ac);
		}

		for (int i = 0; i < g.nitems; i++) {
			struct gitem *it = &g.items[i];
			struct box *b = it->b;
			int area_w = g_area(g.cols, g.ncols, it->c0, it->cspan);
			int area_h = g_area(g.rows, g.nrows, it->r0, it->rspan);
			int ow = b->width + lh__delta_outer_width(b);
			int oh, dx = 0, dy = 0;
			css_fixed len;
			css_unit unit;

			switch (g_align_of(grid, b)) {
			case GA_STRETCH:
				if (css_computed_height(b->style, &len, &unit) ==
						CSS_HEIGHT_AUTO &&
				    !lh__box_is_replace(b) &&
				    it->h < area_h) {
					b->height = area_h -
						lh__delta_outer_height(b);
				}
				break;
			case GA_END:
				dy = area_h - it->h;
				break;
			case GA_CENTER:
				dy = (area_h - it->h) / 2;
				break;
			default:
				break;
			}
			oh = b->height + lh__delta_outer_height(b);
			(void) oh;
			switch (g_justify_of(grid, b)) {
			case GA_END:
				dx = area_w - ow;
				break;
			case GA_CENTER:
				dx = (area_w - ow) / 2;
				break;
			default:
				break;
			}
			if (b->margin[LEFT] == AUTO && b->margin[RIGHT] == AUTO)
				dx = (area_w - ow) / 2;
			else if (b->margin[LEFT] == AUTO)
				dx = area_w - ow;
			if (dx < 0)
				dx = 0;
			if (dy < 0)
				dy = 0;
			b->x = (it->c0 < g.ncols ? g.cols[it->c0].pos :
					grid->padding[LEFT]) + dx +
					lh__non_auto_margin(b, LEFT) +
					b->border[LEFT].width;
			b->y = (it->r0 < g.nrows ? g.rows[it->r0].pos :
					grid->padding[TOP]) + dy +
					lh__non_auto_margin(b, TOP) +
					b->border[TOP].width;
		}

		/* absolutely positioned children: static position at origin */
		for (struct box *b = grid->children; b != NULL; b = b->next) {
			if (!lh__box_is_absolute(b))
				continue;
			b->float_container = b->parent;
			layout_find_dimensions(&content->unit_len_ctx,
					available_width, -1, b, b->style,
					&b->width, &b->height, NULL, NULL,
					NULL, NULL, b->margin, b->padding,
					b->border);
			b->float_container = NULL;
			if (b->width == AUTO)
				b->width = min(b->max_width, available_width) -
						lh__delta_outer_width(b);
			if (b->width < 0)
				b->width = 0;
			g_layout_item(b, available_width, content);
			if (b->height == AUTO)
				b->height = 0;
			b->x = grid->padding[LEFT] + lh__non_auto_margin(b, LEFT) +
					b->border[LEFT].width;
			b->y = grid->padding[TOP] + lh__non_auto_margin(b, TOP) +
					b->border[TOP].width;
		}

		if (grid->height == AUTO)
			grid->height = total_h;
		if (max_height >= 0 && grid->height > max_height)
			grid->height = max_height;
		if (min_height > 0 && grid->height < min_height)
			grid->height = min_height;
	}

	if (getenv("NETSURF_GRID_DEBUG") != NULL) {
		fprintf(stderr, "[grid] %p avail %d: %d cols x %d rows, gaps %d/%d,"
				" w %d h %d\n[grid]  cols:", grid, available_width,
				g.ncols, g.nrows, g.col_gap, g.row_gap,
				grid->width, grid->height);
		for (int i = 0; i < g.ncols && i < 40; i++)
			fprintf(stderr, " %d@%d", g.cols[i].size, g.cols[i].pos);
		fprintf(stderr, "\n[grid]  rows:");
		for (int i = 0; i < g.nrows && i < 40; i++)
			fprintf(stderr, " %d@%d", g.rows[i].size, g.rows[i].pos);
		fprintf(stderr, "\n");
		for (int i = 0; i < g.nitems && i < 40; i++)
			fprintf(stderr, "[grid]  item %d: r%d+%d c%d+%d x %d y %d"
					" w %d h %d\n", i, g.items[i].r0,
					g.items[i].rspan, g.items[i].c0,
					g.items[i].cspan, g.items[i].b->x,
					g.items[i].b->y, g.items[i].b->width,
					g.items[i].b->height);
	}
	NSLOG(layout, DEBUG, "grid %p: %d x %d tracks, %d items, w %d h %d",
			grid, g.ncols, g.nrows, g.nitems, grid->width,
			grid->height);
	g_destroy(&g);
	return ok;
}

void layout_grid_minmax(struct box *grid, html_content *content,
		int *min, int *max)
{
	struct grid g;
	int mn = 0, mx = 0;

	if (!g_setup(&g, grid, content, -1, -1))
		return;

	g_size_tracks(&g, g.cols, g.ncols, -1, g.col_gap, true, false, true);
	for (int i = 0; i < g.ncols; i++)
		mn += g.cols[i].size;
	g_size_tracks(&g, g.cols, g.ncols, -1, g.col_gap, true, false, false);
	for (int i = 0; i < g.ncols; i++)
		mx += g.cols[i].size;
	{
		int vis = g_visible(g.cols, g.ncols);
		if (vis > 1) {
			mn += (vis - 1) * g.col_gap;
			mx += (vis - 1) * g.col_gap;
		}
	}
	if (mx < mn)
		mx = mn;
	*min = mn;
	*max = mx;
	g_destroy(&g);
}
