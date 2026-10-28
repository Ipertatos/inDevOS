/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 *
 * CSS Grid properties (NetSurf box patch).  The complex grid longhands are
 * stored as a normalised copy of their token text in the stylesheet string
 * table; the layout engine interprets them.  row-gap is a normal length.
 */

#include <assert.h>
#include <string.h>
#include <strings.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

#define GRID_BUF 1024

static bool grid_tok_is(const css_token *t, char ch)
{
	return t != NULL && t->type == CSS_TOKEN_CHAR && t->data.len == 1 &&
			t->data.data[0] == ch;
}

/* Serialise tokens up to the end of the declaration ('!' or end) or, if
 * slash is set, up to a '/'.  Returns the trimmed length. */
static size_t grid_collect(const parserutils_vector *vector, int32_t *ctx,
		bool slash, char *buf, size_t size, bool *has_string)
{
	size_t n = 0;
	const css_token *t;

	while ((t = parserutils_vector_peek(vector, *ctx)) != NULL &&
			!grid_tok_is(t, '!') &&
			!(slash && grid_tok_is(t, '/'))) {
		const char *d = (const char *) t->data.data;
		size_t l = (d != NULL) ? t->data.len : 0;

		parserutils_vector_iterate(vector, ctx);

		if (t->type == CSS_TOKEN_S) {
			if (n > 0 && buf[n - 1] != ' ' && n + 1 < size)
				buf[n++] = ' ';
			continue;
		}
		if (n + l + 4 >= size)
			continue;
		if (t->type == CSS_TOKEN_STRING) {
			buf[n++] = '"';
			memcpy(buf + n, d, l);
			n += l;
			buf[n++] = '"';
			if (has_string != NULL)
				*has_string = true;
			continue;
		}
		if (l > 0) {
			memcpy(buf + n, d, l);
			n += l;
		}
		if (t->type == CSS_TOKEN_FUNCTION && (l == 0 || d[l - 1] != '('))
			buf[n++] = '(';
		else if (t->type == CSS_TOKEN_PERCENTAGE &&
				(l == 0 || d[l - 1] != '%'))
			buf[n++] = '%';
	}
	while (n > 0 && buf[n - 1] == ' ')
		n--;
	buf[n] = '\0';
	return n;
}

static css_error grid_emit(css_language *c, css_style *result, opcode_t op,
		const char *buf, size_t n)
{
	lwc_string *str;
	uint32_t snum;
	css_error error;

	if (n == 0)
		return CSS_INVALID;
	if (n == 4 && strncasecmp(buf, "none", 4) == 0)
		return css__stylesheet_style_appendOPV(result, op, 0,
				GRID_STRING_NONE);
	if (lwc_intern_string(buf, n, &str) != lwc_error_ok)
		return CSS_NOMEM;
	error = css__stylesheet_string_add(c->sheet, str, &snum);
	if (error != CSS_OK)
		return error;
	error = css__stylesheet_style_appendOPV(result, op, 0, GRID_STRING_SET);
	if (error != CSS_OK)
		return error;
	return css__stylesheet_style_append(result, snum);
}

static css_error grid_emit_str(css_language *c, css_style *result,
		opcode_t op, const char *s)
{
	return grid_emit(c, result, op, s, strlen(s));
}

/* 1: sole css-wide keyword consumed, 0: none, -1: invalid */
static int grid_flag(css_language *c, const parserutils_vector *vector,
		int32_t *ctx, enum flag_value *fv)
{
	int32_t t = *ctx;
	const css_token *token = parserutils_vector_peek(vector, t);

	if (token == NULL)
		return -1;
	*fv = get_css_flag_value(c, token);
	if (*fv == FLAG_VALUE__NONE)
		return 0;
	parserutils_vector_iterate(vector, &t);
	consumeWhitespace(vector, &t);
	token = parserutils_vector_peek(vector, t);
	if (token != NULL && !grid_tok_is(token, '!'))
		return -1;
	*ctx = t;
	return 1;
}

static css_error grid_flags(css_style *result, enum flag_value fv,
		const opcode_t *ops, int nops)
{
	css_error error = CSS_OK;
	for (int i = 0; i < nops && error == CSS_OK; i++)
		error = css_stylesheet_style_flag_value(result, fv, ops[i]);
	return error;
}

static css_error grid_longhand(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result, opcode_t op)
{
	char buf[GRID_BUF];
	enum flag_value fv;
	int32_t orig = *ctx;
	css_error error;
	size_t n;
	int r = grid_flag(c, vector, ctx, &fv);

	if (r < 0)
		return CSS_INVALID;
	if (r > 0)
		return css_stylesheet_style_flag_value(result, fv, op);
	n = grid_collect(vector, ctx, false, buf, sizeof buf, NULL);
	error = grid_emit(c, result, op, buf, n);
	if (error != CSS_OK)
		*ctx = orig;
	return error;
}

#define GRID_LONGHAND(name, OP) \
css_error css__parse_##name(css_language *c, \
		const parserutils_vector *vector, int32_t *ctx, \
		css_style *result) \
{ \
	return grid_longhand(c, vector, ctx, result, OP); \
}

GRID_LONGHAND(grid_template_columns, CSS_PROP_GRID_TEMPLATE_COLUMNS)
GRID_LONGHAND(grid_template_rows, CSS_PROP_GRID_TEMPLATE_ROWS)
GRID_LONGHAND(grid_template_areas, CSS_PROP_GRID_TEMPLATE_AREAS)
GRID_LONGHAND(grid_auto_columns, CSS_PROP_GRID_AUTO_COLUMNS)
GRID_LONGHAND(grid_auto_rows, CSS_PROP_GRID_AUTO_ROWS)
GRID_LONGHAND(grid_auto_flow, CSS_PROP_GRID_AUTO_FLOW)
GRID_LONGHAND(grid_column_start, CSS_PROP_GRID_COLUMN_START)
GRID_LONGHAND(grid_column_end, CSS_PROP_GRID_COLUMN_END)
GRID_LONGHAND(grid_row_start, CSS_PROP_GRID_ROW_START)
GRID_LONGHAND(grid_row_end, CSS_PROP_GRID_ROW_END)
GRID_LONGHAND(justify_items, CSS_PROP_JUSTIFY_ITEMS)
GRID_LONGHAND(justify_self, CSS_PROP_JUSTIFY_SELF)

/* A lone custom ident (a named line or area) */
static bool grid_is_custom_ident(const char *s)
{
	if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
			*s == '_' || *s == '-'))
		return false;
	if (strchr(s, ' ') != NULL)
		return false;
	return strcasecmp(s, "auto") != 0 && strcasecmp(s, "span") != 0;
}

/* a / b / c / d shorthands for grid placement */
static css_error grid_slash(css_language *c, const parserutils_vector *vector,
		int32_t *ctx, css_style *result, const opcode_t *ops,
		const int *defaults, int nops)
{
	char buf[4][GRID_BUF];
	size_t len[4];
	int parts = 0;
	enum flag_value fv;
	int32_t orig = *ctx;
	const css_token *t;
	css_error error = CSS_OK;
	int r = grid_flag(c, vector, ctx, &fv);

	if (r < 0)
		return CSS_INVALID;
	if (r > 0)
		return grid_flags(result, fv, ops, nops);

	while (parts < nops) {
		len[parts] = grid_collect(vector, ctx, true, buf[parts],
				GRID_BUF, NULL);
		if (len[parts] == 0)
			goto invalid;
		parts++;
		t = parserutils_vector_peek(vector, *ctx);
		if (!grid_tok_is(t, '/'))
			break;
		parserutils_vector_iterate(vector, ctx);
		consumeWhitespace(vector, ctx);
		if (parts == nops)
			goto invalid;
	}

	for (int i = 0; i < nops && error == CSS_OK; i++) {
		if (i < parts) {
			error = grid_emit(c, result, ops[i], buf[i], len[i]);
		} else {
			int d = defaults[i];
			/* grid-area: column-end falls back to column-start,
			 * which itself falls back to row-start */
			while (d >= parts && d >= 0 && defaults[d] != d)
				d = defaults[d];
			if (d >= 0 && d < parts && grid_is_custom_ident(buf[d]))
				error = grid_emit(c, result, ops[i],
						buf[d], len[d]);
			else
				error = grid_emit_str(c, result, ops[i], "auto");
		}
	}
	if (error != CSS_OK)
		*ctx = orig;
	return error;
invalid:
	*ctx = orig;
	return CSS_INVALID;
}

css_error css__parse_grid_column(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const opcode_t ops[] = { CSS_PROP_GRID_COLUMN_START,
			CSS_PROP_GRID_COLUMN_END };
	static const int defaults[] = { -1, 0 };
	return grid_slash(c, vector, ctx, result, ops, defaults, 2);
}

css_error css__parse_grid_row(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const opcode_t ops[] = { CSS_PROP_GRID_ROW_START,
			CSS_PROP_GRID_ROW_END };
	static const int defaults[] = { -1, 0 };
	return grid_slash(c, vector, ctx, result, ops, defaults, 2);
}

css_error css__parse_grid_area(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const opcode_t ops[] = { CSS_PROP_GRID_ROW_START,
			CSS_PROP_GRID_COLUMN_START, CSS_PROP_GRID_ROW_END,
			CSS_PROP_GRID_COLUMN_END };
	static const int defaults[] = { -1, 0, 0, 1 };
	return grid_slash(c, vector, ctx, result, ops, defaults, 4);
}

static bool grid_has_word(const char *s, const char *w)
{
	size_t wl = strlen(w);
	for (; *s != '\0'; s++)
		if (strncasecmp(s, w, wl) == 0)
			return true;
	return false;
}

/* Remove the words auto-flow / dense from s; returns whether dense seen */
static bool grid_strip_flow(char *s, size_t *len)
{
	bool dense = false;
	char out[GRID_BUF];
	size_t n = 0;
	char *p = s;

	while (*p != '\0') {
		char *e = p;
		size_t wl;
		while (*e != '\0' && *e != ' ')
			e++;
		wl = e - p;
		if (wl == 9 && strncasecmp(p, "auto-flow", 9) == 0) {
			/* drop */
		} else if (wl == 5 && strncasecmp(p, "dense", 5) == 0) {
			dense = true;
		} else if (wl > 0) {
			if (n > 0)
				out[n++] = ' ';
			memcpy(out + n, p, wl);
			n += wl;
		}
		p = (*e == ' ') ? e + 1 : e;
	}
	out[n] = '\0';
	memcpy(s, out, n + 1);
	*len = n;
	return dense;
}

static css_error grid_template_common(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result, bool full)
{
	static const opcode_t ops[] = { CSS_PROP_GRID_TEMPLATE_ROWS,
			CSS_PROP_GRID_TEMPLATE_COLUMNS,
			CSS_PROP_GRID_TEMPLATE_AREAS, CSS_PROP_GRID_AUTO_FLOW,
			CSS_PROP_GRID_AUTO_ROWS, CSS_PROP_GRID_AUTO_COLUMNS };
	char rows[GRID_BUF], cols[GRID_BUF];
	size_t rl, cl = 0;
	bool has_string = false;
	enum flag_value fv;
	int32_t orig = *ctx;
	const css_token *t;
	css_error error;
	const char *flow = "row", *arows = "auto", *acols = "auto";
	int r = grid_flag(c, vector, ctx, &fv);

	if (r < 0)
		return CSS_INVALID;
	if (r > 0)
		return grid_flags(result, fv, ops, full ? 6 : 3);

	rl = grid_collect(vector, ctx, true, rows, GRID_BUF, &has_string);
	t = parserutils_vector_peek(vector, *ctx);
	if (grid_tok_is(t, '/')) {
		parserutils_vector_iterate(vector, ctx);
		consumeWhitespace(vector, ctx);
		cl = grid_collect(vector, ctx, false, cols, GRID_BUF, NULL);
		if (cl == 0)
			goto invalid;
	} else {
		strcpy(cols, "none");
		cl = 4;
	}
	if (rl == 0)
		goto invalid;

	if (full && grid_has_word(rows, "auto-flow")) {
		bool dense = grid_strip_flow(rows, &rl);
		flow = dense ? "row dense" : "row";
		if (rl > 0)
			arows = rows;
		error = grid_emit_str(c, result, ops[0], "none");
		if (error == CSS_OK)
			error = grid_emit(c, result, ops[1], cols, cl);
		if (error == CSS_OK)
			error = grid_emit_str(c, result, ops[2], "none");
	} else if (full && grid_has_word(cols, "auto-flow")) {
		bool dense = grid_strip_flow(cols, &cl);
		flow = dense ? "column dense" : "column";
		if (cl > 0)
			acols = cols;
		error = grid_emit(c, result, ops[0], rows, rl);
		if (error == CSS_OK)
			error = grid_emit_str(c, result, ops[1], "none");
		if (error == CSS_OK)
			error = grid_emit_str(c, result, ops[2], "none");
	} else {
		error = grid_emit(c, result, ops[0], rows, rl);
		if (error == CSS_OK)
			error = grid_emit(c, result, ops[1], cols, cl);
		if (error == CSS_OK) {
			if (has_string)
				error = grid_emit(c, result, ops[2], rows, rl);
			else
				error = grid_emit_str(c, result, ops[2], "none");
		}
	}
	if (full) {
		if (error == CSS_OK)
			error = grid_emit_str(c, result, ops[3], flow);
		if (error == CSS_OK)
			error = grid_emit_str(c, result, ops[4], arows);
		if (error == CSS_OK)
			error = grid_emit_str(c, result, ops[5], acols);
	}
	if (error != CSS_OK)
		*ctx = orig;
	return error;
invalid:
	*ctx = orig;
	return CSS_INVALID;
}

css_error css__parse_grid_template(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return grid_template_common(c, vector, ctx, result, false);
}

css_error css__parse_grid(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return grid_template_common(c, vector, ctx, result, true);
}

css_error css__parse_gap(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	static const opcode_t ops[] = { CSS_PROP_ROW_GAP, CSS_PROP_COLUMN_GAP };
	enum flag_value fv;
	int32_t orig = *ctx, start;
	const css_token *t;
	css_error error;
	int r = grid_flag(c, vector, ctx, &fv);

	if (r < 0)
		return CSS_INVALID;
	if (r > 0)
		return grid_flags(result, fv, ops, 2);

	start = *ctx;
	error = css__parse_row_gap(c, vector, ctx, result);
	if (error != CSS_OK) {
		*ctx = orig;
		return error;
	}
	consumeWhitespace(vector, ctx);
	t = parserutils_vector_peek(vector, *ctx);
	if (t != NULL && !grid_tok_is(t, '!')) {
		error = css__parse_column_gap(c, vector, ctx, result);
	} else {
		int32_t c2 = start;
		error = css__parse_column_gap(c, vector, &c2, result);
	}
	if (error != CSS_OK)
		*ctx = orig;
	return error;
}
