#
# Makefile for optimizer
#
# src/backend/px_optimizer/libnaucrates/src/base/Makefile
#

subdir = src/backend/px_optimizer/libnaucrates/src/base
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

OBJS        = CDatumBoolGPDB.o \
              CDatumGenericGPDB.o \
              CDatumInt2GPDB.o \
              CDatumInt4GPDB.o \
              CDatumInt8GPDB.o \
              CDatumOidGPDB.o \
              CQueryToDXLResult.o \
              IDatum.o

include $(top_srcdir)/src/backend/common.mk

