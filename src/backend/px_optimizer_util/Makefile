#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for px_optimizer_util
#
#-------------------------------------------------------------------------

subdir = src/backend/px_optimizer_util
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(srcdir)/px_optimizer_util.mk

SUBDIRS = config translate relcache utils

OBJS = CGPOptimizer.o px_wrappers.o

include $(top_srcdir)/src/backend/common.mk
