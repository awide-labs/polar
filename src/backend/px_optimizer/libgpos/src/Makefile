#
# Makefile for optimizer
#
# src/backend/px_optimizer/Makefile
#

subdir = src/backend/px_optimizer/libgpos/src
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

SUBDIRS     = common error io memory string task
OBJS        = _api.o utils.o

include $(top_srcdir)/src/backend/common.mk

