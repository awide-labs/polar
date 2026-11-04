#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/plan
#
# IDENTIFICATION
#    src/backend/optimizer/plan/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/plan
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	analyzejoins.o \
	createplan.o \
	initsplan.o \
	planagg.o \
	planmain.o \
	planner.o \
	setrefs.o \
	subselect.o

ifeq ($(enable_polar_px),yes)
OBJS += px_opt.o
endif

include $(top_srcdir)/src/backend/common.mk
