#
# Makefile for optimizer
#
# src/backend/px_optimizer/libgpopt/src/engine/Makefile
#

subdir = src/backend/px_optimizer/libgpopt/src/engine
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

OBJS        = CEngine.o \
              CEnumeratorConfig.o \
              CPartialPlan.o \
              CStatisticsConfig.o

include $(top_srcdir)/src/backend/common.mk

