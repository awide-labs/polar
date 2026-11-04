#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for gpopt
#
#-------------------------------------------------------------------------

subdir = src/backend/px_optimizer_util/utils
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer_util/px_optimizer_util.mk

OBJS = COptTasks.o CConstExprEvaluatorProxy.o CMemoryPoolPalloc.o CMemoryPoolPallocManager.o RelationWrapper.o funcs.o

include $(top_srcdir)/src/backend/common.mk
