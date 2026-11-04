#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for motion
#
#-------------------------------------------------------------------------


subdir = src/backend/px/motion
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -Wno-declaration-after-statement -I$(libpq_srcdir) $(CPPFLAGS)

OBJS = px_motion.o tupchunklist.o tupser.o  \
	ic_common.o ic_tcp.o ic_udpifc.o htupfifo.o tupleremap.o

include $(top_srcdir)/src/backend/common.mk
