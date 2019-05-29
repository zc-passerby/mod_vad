include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_vad

mod_LTLIBRARIES = mod_vad.la
mod_vad_la_SOURCES  = mod_vad.c
mod_vad_la_CFLAGS   = $(AM_CFLAGS)
mod_vad_la_CPPFLAGS = $(AM_CPPFLAGS)
mod_vad_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_vad_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
