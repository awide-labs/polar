#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for dispatcher
#
#-------------------------------------------------------------------------

subdir = src/backend/px/dispatcher
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -I$(srcdir) -I$(libpq_srcdir) -I$(top_srcdir)/src/port -I$(top_srcdir)/src/backend/utils/misc

OBJS = px_conn.o px_disp.o px_disp_async.o px_dispatchresult.o px_disp_query.o px_gang.o px_gang_async.o px_adps_coordinator_thread.o px_adaptive_paging.o
include $(top_srcdir)/src/backend/common.mk
