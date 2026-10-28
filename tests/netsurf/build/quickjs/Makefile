#
# NetSurf javascript source file inclusion: QuickJS backend
#
# Included by javascript/Makefile
#

content/handlers/javascript/quickjs/qjs.c: \
	$(OBJROOT)/quickjs/dom.js.inc

# Generator for the C include representing the DOM prelude dom.js
$(OBJROOT)/quickjs/dom.js.inc: content/handlers/javascript/quickjs/dom.js $(TOOLROOT)/xxd
	$(Q)$(MKDIR) -p $(OBJROOT)/quickjs
	$(VQ)echo "     XXD: $<"
	$(Q)$(TOOLROOT)/xxd -i $< $@.tmp
	$(Q)sed -e 's/content_handlers_javascript_quickjs_dom_js/dom_js/' $@.tmp > $@

S_JAVASCRIPT += content.c quickjs/qjs.c
