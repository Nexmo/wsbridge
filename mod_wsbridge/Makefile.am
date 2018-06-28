include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_wsbridge

# nexmo
if HAVE_WEBSOCKETS
mod_LTLIBRARIES = mod_wsbridge.la
mod_wsbridge_la_SOURCES  = mod_wsbridge.c
mod_wsbridge_la_CFLAGS   = $(AM_CFLAGS) $(WEBSOCKETS_CFLAGS) -I.
mod_wsbridge_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_wsbridge_la_LDFLAGS  = -avoid-version -module -no-undefined -shared $(WEBSOCKETS_LIBS)
endif

