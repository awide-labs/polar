#
# Makefile for optimizer
#
# src/backend/px_optimizer/libgpopt/src/Makefile
#

subdir = src/backend/px_optimizer/libgpopt/src
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

SUBDIRS     = base engine eval mdcache metadata minidump operators optimizer search translate xforms
OBJS        = exception.o init.o

include $(top_srcdir)/src/backend/common.mk

