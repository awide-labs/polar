#!/bin/bash

# build-rpm.sh
#	  Use shell script to build .rpm packages.
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
#	  package/rpm/build-rpm.sh

RPM_MACROS=$HOME/.rpmmacros
TOP_DIR=`pwd`/.self_rpm_create

release=${1:-$(date +"%Y%m%d%H%M%S")}

rm -f $RPM_MACROS
echo "%_topdir $TOP_DIR" > $RPM_MACROS
echo "%_release $release" >> $RPM_MACROS

rm -rf $TOP_DIR
RELEASE=$release rpmbuild -bb --buildroot $TOP_DIR/BUILD PolarDB.spec;st=$?

if [ "$st" != 0 ]; then
    exit $st
fi

cp -rf $TOP_DIR/RPMS/* .
