# src/bin/polar_tools/Makefile 

PGFILEDESC = "polar_tools"
PGAPPICON=win32

subdir = src/bin/polar_tools
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
LDFLAGS_INTERNAL += -Wl,--as-needed -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport) $(polar_libvfs)

OBJS = polar_tools.o \
	   block_header_dump.o \
	   control_data_change.o \
	   logindex_meta_dump.o \
	   logindex_bloom_dump.o \
	   logindex_table_dump.o \
	   logindex_page_dump.o \
	   bloomfilter.o \
	   xlogreader.o

CPPFLAGS_XLOGREADER := $(CPPFLAGS) -DFRONTEND 

all: polar_tools

polar_tools: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

xlogreader.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .

xlogreader.o: xlogreader.c
	$(CC) $(CFLAGS) $(CPPFLAGS_XLOGREADER) -c -o $@ $<

install: all installdirs
	$(INSTALL_PROGRAM) polar_tools$(X) '$(DESTDIR)$(bindir)/polar_tools$(X)'
	$(INSTALL_PROGRAM) dbatools.sql$(X) '$(DESTDIR)$(bindir)/dbatools.sql$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/polar_tools$(X)'

clean distclean maintainer-clean:
	rm -f polar_tools$(X) $(OBJS) xlogreader.c

