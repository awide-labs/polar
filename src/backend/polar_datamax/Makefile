#
## Makefile for the datamax module
#
## src/backend/polar_datamax/Makefile
#
#
#subdir = src/backend/polar_datamax
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

subdir = src/backend/polar_datamax

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = polar_datamax.o

include $(top_srcdir)/src/backend/common.mk

polar_datamax.o : polar_datamax.c
