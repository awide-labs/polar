#
# Makefile for optimizer
#
# src/backend/px_optimizer/libgpopt/src/translate/Makefile
#

subdir = src/backend/px_optimizer/libgpopt/src/translate
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer/px_optimizer.mk

OBJS        = CTranslatorDXLToExpr.o \
              CTranslatorDXLToExprUtils.o \
              CTranslatorExprToDXL.o \
              CTranslatorExprToDXLUtils.o

include $(top_srcdir)/src/backend/common.mk

