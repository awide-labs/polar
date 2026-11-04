#
# Makefile for optimizer
#
# src/backend/px_optimizer/Makefile
#

subdir = src/backend/px_optimizer/libnaucrates/src
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

SUBDIRS     = base md operators parser statistics xml
OBJS        = CCostModelConfigSerializer.o \
              CDXLUtils.o \
              CIdGenerator.o \
              exception.o \
              init.o \
              traceflags.o

include $(top_srcdir)/src/backend/common.mk

