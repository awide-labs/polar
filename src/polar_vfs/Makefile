#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the polar vfs subsystem
#
# This makefile generates two outputs:
#
#	polarvfs.a - contains object files with FRONTEND defined,
#		for use by client application and libraries
#
#	polarvfs_srv.a - contains object files without FRONTEND defined,
#		for use only by the backend binaries
#
# IDENTIFICATION
#    src/polar_vfs/Makefile
#
#-------------------------------------------------------------------------

subdir = src/polar_vfs
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(top_builddir)/src/port -DFRONTEND $(CPPFLAGS) $(polar_flagpfsd)
LDFLAGS_SL += $(filter -lm, $(LIBS))

SHLIB_LINK = $(libpq) $(polar_libpfsd)

OBJS_COMMON = polar_bufferio.o polar_directio.o polar_pfsd.o polar_vfs_interface.o

OBJS_FE = $(OBJS_COMMON) polar_vfs_fe.o
# foo_srv.o and foo.o are both built from foo.c, but only foo.o has -DFRONTEND
OBJS_SRV = $(OBJS_COMMON:%.o=%_srv.o) polar_vfs_srv.o

PGFILEDESC = "polar_vfs - standard polar virtual file system plugin"
NAME = polar_vfs
OBJS = $(OBJS_SRV)
DATA = polar_vfs.control polar_vfs--1.0.sql
EXTENSION = polar_vfs

libpolarvfs.a: $(OBJS_FE)
	rm -f $@
	$(AR) $(AROPT) $@ $^

%_srv.o: %.c
	$(CC) $(CFLAGS) $(subst -DFRONTEND,, $(CPPFLAGS)) -c $< -o $@

all: all-shared-lib libpolarvfs.a $(OBJS_SRV)

include $(top_srcdir)/src/Makefile.shlib

install-data: installdirs
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DATA)) '$(DESTDIR)$(datadir)/extension/'

install: all install-lib install-data
	$(INSTALL_STLIB) libpolarvfs.a '$(DESTDIR)$(libdir)/libpolarvfs.a'

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension'
	$(MKDIR_P) '$(DESTDIR)$(libdir)'

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(libdir)/libpolarvfs.a'

clean distclean maintainer-clean: clean-lib
	rm -f libpolarvfs.a $(OBJS_FE) $(OBJS_SRV)