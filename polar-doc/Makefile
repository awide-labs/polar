# Makefile
#	  Makefile for VuePress documentation website of PolarDB.
#
# Copyright (c) 2024, Alibaba Group Holding Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# IDENTIFICATION
#	  polar-doc/Makefile

subdir = polar-doc
top_builddir = ..
include $(top_builddir)/src/Makefile.global

all: docs-build

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(htmldir)'/polar-doc

install:
	cp -r ./docs/.vuepress/dist/* '$(DESTDIR)$(htmldir)'/polar-doc/

uninstall:
	rm -rf '$(DESTDIR)$(htmldir)'/polar-doc/*

clean:
	rm -rf ./docs/.vuepress/dist
	rm -rf ./docs/.vuepress/.cache
	rm -rf ./docs/.vuepress/.temp

distclean: clean
	rm -rf ./node_modules

maintainer-clean: distclean

deps:
	pnpm install --frozen-lockfile --registry=https://registry.npmmirror.com &>/dev/null

docs-build: deps
	pnpm run docs:build

docs-dev: deps
	pnpm run docs:dev

docs-format: deps
	pnpm run prettier:format
	pnpm run prettier:format_root
	python3 docs/zh/features/generate-version-badge.py
	python3 docs/zh/features/generate-index.py
