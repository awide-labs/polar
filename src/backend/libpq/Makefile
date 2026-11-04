#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (backend half of libpq interface)
#
# IDENTIFICATION
#    src/backend/libpq/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) -I$(top_srcdir)/src/port $(CPPFLAGS)

# be-fsstubs is here for historical reasons, probably belongs elsewhere

OBJS = \
	auth-sasl.o \
	auth-scram.o \
	auth.o \
	be-fsstubs.o \
	be-secure-common.o \
	be-secure.o \
	crypt.o \
	hba.o \
	ifaddr.o \
	pqcomm.o \
	pqformat.o \
	pqmq.o \
	pqsignal.o \
	polar_network_stats.o

# POLAR px
OBJS += \
		fe-trace.o \
		fe-protocol3.o \
		fe-connect.o \
		fe-auth-scram.o \
		libpq-events.o \
		fe-exec.o \
		pqexpbuffer.o \
		fe-auth.o \
		fe-misc.o \
		fe-secure.o \
		$(filter getpeereid.o, $(LIBOBJS))

ifeq ($(with_ssl),openssl)
OBJS += be-secure-openssl.o
OBJS += fe-secure-common.o \
		fe-secure-openssl.o \
		polar_px_auth_tls.o
endif

ifeq ($(with_gssapi),yes)
OBJS += be-gssapi-common.o be-secure-gssapi.o
OBJS += fe-gssapi-common.o fe-secure-gssapi.o
endif

fe-trace.c fe-protocol3.c fe-connect.c fe-auth-scram.c libpq-events.c fe-exec.c pqexpbuffer.c fe-auth.c fe-misc.c fe-secure.c fe-secure-common.c fe-secure-openssl.c fe-gssapi-common.c fe-secure-gssapi.c: % : $(top_srcdir)/src/interfaces/libpq/%
	rm -f $@ && $(LN_S) $< .

getpeereid.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

fe-connect.o: fe-connect.c $(top_builddir)/src/port/pg_config_paths.h

$(top_builddir)/src/port/pg_config_paths.h:
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

clean distclean: clean-symlinks

clean-symlinks:
	rm -f fe-trace.c fe-protocol3.c fe-connect.c fe-auth-scram.c libpq-events.c fe-exec.c pqexpbuffer.c fe-auth.c fe-misc.c fe-secure.c fe-secure-common.c fe-secure-openssl.c fe-gssapi-common.c fe-secure-gssapi.c getpeereid.c

include $(top_srcdir)/src/backend/common.mk
