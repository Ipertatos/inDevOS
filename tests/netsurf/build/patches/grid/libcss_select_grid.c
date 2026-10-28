/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 *
 * CSS Grid properties (NetSurf box patch): row-gap and the string-valued
 * grid longhands.
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

css_error css__cascade_row_gap(uint32_t opv, css_style *style,
		css_select_state *state)
{
	return css__cascade_length_normal(opv, style, state, set_row_gap);
}

css_error css__set_row_gap_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_row_gap(style, hint->status,
			hint->data.length.value, hint->data.length.unit);
}

css_error css__initial_row_gap(css_select_state *state)
{
	return set_row_gap(state->computed, CSS_ROW_GAP_NORMAL,
			INTTOFIX(1), CSS_UNIT_EM);
}

css_error css__copy_row_gap(
		const css_computed_style *from,
		css_computed_style *to)
{
	css_fixed length = 0;
	css_unit unit = CSS_UNIT_PX;
	uint8_t type = get_row_gap(from, &length, &unit);

	if (from == to) {
		return CSS_OK;
	}

	return set_row_gap(to, type, length, unit);
}

css_error css__compose_row_gap(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	css_fixed length = 0;
	css_unit unit = CSS_UNIT_PX;
	uint8_t type = get_row_gap(child, &length, &unit);

	return css__copy_row_gap(
			type == CSS_ROW_GAP_INHERIT ? parent : child,
			result);
}

#define GRID_STRING_PROP(name) \
css_error css__cascade_##name(uint32_t opv, css_style *style, \
		css_select_state *state) \
{ \
	uint16_t value = CSS_GRID_STRING_INHERIT; \
	lwc_string *str = NULL; \
	if (hasFlagValue(opv) == false) { \
		value = CSS_GRID_STRING_SET; \
		if (getValue(opv) == GRID_STRING_SET) { \
			css__stylesheet_string_get(style->sheet, \
					*((css_code_t *) style->bytecode), \
					&str); \
			advance_bytecode(style, sizeof(css_code_t)); \
		} \
	} \
	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state, \
			getFlagValue(opv))) { \
		return set_##name(state->computed, value, str); \
	} \
	return CSS_OK; \
} \
css_error css__set_##name##_from_hint(const css_hint *hint, \
		css_computed_style *style) \
{ \
	css_error error = set_##name(style, hint->status, hint->data.string); \
	if (hint->data.string != NULL) \
		lwc_string_unref(hint->data.string); \
	return error; \
} \
css_error css__initial_##name(css_select_state *state) \
{ \
	return set_##name(state->computed, CSS_GRID_STRING_SET, NULL); \
} \
css_error css__copy_##name(const css_computed_style *from, \
		css_computed_style *to) \
{ \
	lwc_string *str = NULL; \
	uint8_t type = get_##name(from, &str); \
	if (from == to) \
		return CSS_OK; \
	return set_##name(to, type, str); \
} \
css_error css__compose_##name(const css_computed_style *parent, \
		const css_computed_style *child, \
		css_computed_style *result) \
{ \
	lwc_string *str = NULL; \
	uint8_t type = get_##name(child, &str); \
	return css__copy_##name(type == CSS_GRID_STRING_INHERIT ? \
			parent : child, result); \
}

GRID_STRING_PROP(grid_template_columns)
GRID_STRING_PROP(grid_template_rows)
GRID_STRING_PROP(grid_template_areas)
GRID_STRING_PROP(grid_auto_columns)
GRID_STRING_PROP(grid_auto_rows)
GRID_STRING_PROP(grid_auto_flow)
GRID_STRING_PROP(grid_column_start)
GRID_STRING_PROP(grid_column_end)
GRID_STRING_PROP(grid_row_start)
GRID_STRING_PROP(grid_row_end)
GRID_STRING_PROP(justify_items)
GRID_STRING_PROP(justify_self)
