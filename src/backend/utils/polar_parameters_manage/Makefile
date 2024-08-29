#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/error
#
# IDENTIFICATION
#    src/backend/utils/error/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/polar_parameters_manage
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	polar_cluster_settings.o polar_guc_flag_check.o

include $(top_srcdir)/src/backend/common.mk

# copy all parameters sample to data directory
install:
	$(INSTALL_DATA) $(srcdir)/polar_parameters_sample/polar_settings\.conf\.*\.sample '$(DESTDIR)$(datadir)/'