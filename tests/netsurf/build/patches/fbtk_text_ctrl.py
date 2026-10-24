# Ctrl+A / Ctrl+C / Ctrl+V for the fbtk text widget (NetSurf URL bar).
# Ctrl+A marks the whole text: the next typed char or Backspace replaces it,
# cursor keys just drop the mark. Ctrl+C copies the text, Ctrl+V pastes it
# (widget-local clipboard). Idempotent.
import sys
p=sys.argv[1]; s=open(p).read()
if 'fbtk_text_clip' in s: sys.exit(0)
s=s.replace("""static int
text_input(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{""","""static char *fbtk_text_clip;     /* Ctrl+C / Ctrl+V buffer */
static bool fbtk_text_all;       /* Ctrl+A: whole text marked */

static void text_clear_all(fbtk_widget_t *widget)
{
	widget->u.text.idx = 0;
	widget->u.text.len = 0;
	widget->u.text.text[0] = '\\0';
	widget->u.text.width = 0;
}

static int
text_input(fbtk_widget_t *widget, fbtk_callback_info *cbi)
{""",1)
old="""	switch (value) {
	case NSFB_KEY_BACKSPACE:
		if (widget->u.text.idx <= 0)"""
assert old in s
s=s.replace(old,"""	if (fbtk_text_all && value != NSFB_KEY_LSHIFT &&
	    value != NSFB_KEY_RSHIFT && value != NSFB_KEY_LCTRL &&
	    value != NSFB_KEY_RCTRL) {
		fbtk_text_all = false;
		if (value == NSFB_KEY_BACKSPACE || value == NSFB_KEY_DELETE ||
		    (!(modifier & (FBTK_MOD_LCTRL | FBTK_MOD_RCTRL)) &&
		     value >= 32 && value < 127) ||
		    ((modifier & (FBTK_MOD_LCTRL | FBTK_MOD_RCTRL)) &&
		     value == NSFB_KEY_v)) {
			text_clear_all(widget);
			caret_moved = true;
			if (value == NSFB_KEY_BACKSPACE || value == NSFB_KEY_DELETE)
				goto text_done;
		}
	}

	switch (value) {
	case NSFB_KEY_BACKSPACE:
		if (widget->u.text.idx <= 0)""",1)
old="""			if (value == NSFB_KEY_u) {"""
assert old in s
s=s.replace(old,"""			if (value == NSFB_KEY_a) {
				fbtk_text_all = true;
				widget->u.text.idx = widget->u.text.len;
				caret_moved = true;
			} else if (value == NSFB_KEY_c) {
				free(fbtk_text_clip);
				fbtk_text_clip = strdup(widget->u.text.text);
			} else if (value == NSFB_KEY_v && fbtk_text_clip) {
				size_t n = strlen(fbtk_text_clip);
				temp = realloc(widget->u.text.text,
						widget->u.text.len + n + 1);
				if (temp != NULL) {
					widget->u.text.text = temp;
					memmove(temp + widget->u.text.idx + n,
						temp + widget->u.text.idx,
						widget->u.text.len - widget->u.text.idx + 1);
					memcpy(temp + widget->u.text.idx,
						fbtk_text_clip, n);
					widget->u.text.idx += n;
					widget->u.text.len += n;
					fb_font_width(&font_style, temp,
						widget->u.text.len,
						&widget->u.text.width);
					caret_moved = true;
				}
			}
			if (value == NSFB_KEY_u) {""",1)
old="""	if (caret_moved) {
		fb_font_width(&font_style, widget->u.text.text,
				widget->u.text.idx, &widget->u.text.idx_offset);"""
assert old in s
s=s.replace(old,"text_done:\n"+old,1)
open(p,'w').write(s)
print("patched",p)
