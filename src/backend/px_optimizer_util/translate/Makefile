#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for px_optimizer_util
#
#-------------------------------------------------------------------------

subdir = src/backend/px_optimizer_util/translate
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

include $(top_srcdir)/src/backend/px_optimizer_util/px_optimizer_util.mk

OBJS =  CMappingColIdVar.o \
		CMappingVarColId.o \
		CMappingElementColIdParamId.o \
		CMappingColIdVarPlStmt.o \
		CDXLTranslateContextBaseTable.o \
		CCTEListEntry.o \
		CContextDXLToPlStmt.o \
		CDXLTranslateContext.o \
		CQueryMutators.o \
		CTranslatorScalarToDXL.o \
		CTranslatorDXLToScalar.o \
		CTranslatorUtils.o \
		CTranslatorRelcacheToDXL.o \
		CContextQueryToDXL.o \
		CTranslatorQueryToDXL.o \
		CTranslatorDXLToPlStmt.o \
		CPartPruneStepsBuilder.o

include $(top_srcdir)/src/backend/common.mk
