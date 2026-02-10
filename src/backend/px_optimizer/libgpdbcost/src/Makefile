#
# Makefile for optimizer
#
# src/backend/px_optimizer/libgpdbcost/src/Makefile
#

subdir = src/backend/px_optimizer/libgpdbcost/src
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

OBJS        = CCostModelGPDB.o \
              CCostModelPolarDB.o \
              CCostModelParamsGPDB.o \
              CCostModelParamsPolarDB.o \
              ICostModel.o

include $(top_srcdir)/src/backend/common.mk

