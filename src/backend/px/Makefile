#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for px (PX specific routines)
#
#-------------------------------------------------------------------------


subdir = src/backend/px
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

SUBDIRS := motion dispatcher

OBJS = \
	   px_vars.o \
	   px_util.o \
	   px_hash.o \
	   px_cat.o \
	   px_mutate.o \
	   px_plan.o \
	   px_llize.o \
	   px_srlz.o \
	   px_threadlog.o \
	   px_snapshot.o \
	   # px_timeout.o \

ifeq ($(PORTNAME),cygwin)
.LIBPATTERNS := $(filter-out %.so,$(.LIBPATTERNS))
endif



ALLOBJS = $(OBJS) $(SUBDIROBJS)

include $(top_srcdir)/src/backend/common.mk
