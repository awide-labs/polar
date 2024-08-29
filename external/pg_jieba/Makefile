# external/pg_jieba/Makefile
EXTENSION = pg_jieba
DATA = pg_jieba--1.1.0.sql

MODULE_big = $(EXTENSION)
OBJS = pg_jieba.o

PG_CPPFLAGS = -Ilibjieba/deps -Ilibjieba/include -w -std=c++14
SHLIB_LINK += -lstdc++

REGRESS = pg_jieba
REGRESS_WITHOUT_PRELOAD = without_preload
EXTRA_OPTS = --temp-config pg_jieba.conf
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/pg_jieba
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

install_jieba:
	$(MKDIR_P) $(DESTDIR)$(datadir)/tsearch_data
	$(INSTALL_DATA) libjieba/dict/jieba.dict.utf8 $(DESTDIR)$(datadir)/tsearch_data/jieba.dict.utf8
	$(INSTALL_DATA) libjieba/dict/hmm_model.utf8 $(DESTDIR)$(datadir)/tsearch_data/jieba.hmm_model.utf8
	$(INSTALL_DATA) libjieba/dict/user.dict.utf8 $(DESTDIR)$(datadir)/tsearch_data/jieba.user.dict.utf8
	$(INSTALL_DATA) libjieba/dict/idf.utf8 $(DESTDIR)$(datadir)/tsearch_data/jieba.idf.utf8
	$(INSTALL_DATA) libjieba/dict/stop_words.utf8 $(DESTDIR)$(datadir)/tsearch_data/jieba.stop

uninstall_jieba:
	rm -rf $(DESTDIR)$(datadir)/tsearch_data/jieba.*

install: install_jieba
uninstall: uninstall_jieba

check:
	$(pg_regress_check) $(REGRESS_OPTS) $(EXTRA_OPTS) $(REGRESS)
	$(pg_regress_check) $(REGRESS_OPTS) $(REGRESS_WITHOUT_PRELOAD)

COMPILE.cxx.bc := $(filter-out -flto=thin, $(COMPILE.cxx.bc))