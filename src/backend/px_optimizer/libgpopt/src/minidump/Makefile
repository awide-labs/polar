#
# Makefile for optimizer
#
# src/backend/px_optimizer/libgpopt/src/minidump/Makefile
#

subdir = src/backend/px_optimizer/libgpopt/src/minidump
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

OBJS        = CDXLMinidump.o \
              CMetadataAccessorFactory.o \
              CMiniDumperDXL.o \
              CMinidumperUtils.o \
              CSerializableMDAccessor.o \
              CSerializableOptimizerConfig.o \
              CSerializablePlan.o \
              CSerializableQuery.o \
              CSerializableStackTrace.o

include $(top_srcdir)/src/backend/common.mk

