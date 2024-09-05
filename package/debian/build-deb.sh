#!/bin/bash

# build-deb.sh
#	  Use shell script to build .deb packages.
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
#	  package/debian/build-deb.sh

build_date=${1:-$(date +"%Y%m%d%H%M%S")}
code_commit=$(git rev-parse HEAD || echo unknown)
code_branch=$(git rev-parse --abbrev-ref HEAD || echo unknown)
polar_commit=$(git rev-parse --short=8 HEAD || echo unknown)
pg_version=$(grep AC_INIT ../../configure.ac | awk -F'[][]' '{print $4}')
polar_majorversion=$(echo ${pg_version})
polar_minorversion=$(grep '^POLAR_MINORVERSION' ../../configure.ac | cut -d '"' -f2)
polar_release_date=$(grep '^POLAR_RELEASE_DATE' ../../configure.ac | cut -d '"' -f2)
polar_version=${polar_majorversion}.${polar_minorversion}
polar_pg_version=${pg_version}.${polar_minorversion}

package=PolarDB
debpkgname=$(grep 'Package:' ./control | awk '{print $2}')
distname=$(grep '^ID=' /etc/os-release | awk -F'=' '{print $2}')
distversion=$(grep 'VERSION_ID=' /etc/os-release | awk -F\" '{print $2}')
arch=$(dpkg --print-architecture)

pkgname=${package}_${polar_version}-${polar_commit}-${distname}${distversion}_${arch}
rm -rf ./${pkgname}
mkdir -p ${pkgname}/DEBIAN

cat ./control >> ${pkgname}/DEBIAN/control
echo 'Version: '${polar_version}'-'${polar_commit}'' >> ${pkgname}/DEBIAN/control
echo 'Architecture: '${arch}'' >> ${pkgname}/DEBIAN/control

prefix=/u01/polardb_pg
buildroot=$(pwd)
cd ../../
export COPT="-Wno-error ${COPT-}"
./build.sh --ec="--prefix=${buildroot}/${pkgname}${prefix} --with-pfsd" --debug=off --ni
cd ${buildroot}

polar_install_dependency()
{
  target_dir=${1}

  # create link for lib inside lib
  cd ${target_dir}/lib/
  ln -sf ../lib ./lib

  cd ${target_dir}

  # generate list of .so files
  # collect all the executable binaries, scripts and shared libraries
  binfiles=`find ${target_dir}/bin`
  libfiles=`find ${target_dir}/lib`
  filelist=${binfiles}$'\n'${libfiles}
  exelist=`echo $filelist | xargs -r file | egrep -v ":.* (commands|script)" | \
      grep ":.*executable" | cut -d: -f1`
  scriptlist=`echo $filelist | xargs -r file | \
      egrep ":.* (commands|script)" | cut -d: -f1`
  liblist=`echo $filelist | xargs -r file | \
      grep ":.*shared object" | cut -d : -f1`

  # dependency list of the executable binaries and shared libraries
  cp /dev/null mytmpfilelist
  cp /dev/null mytmpfilelist2

  # put PolarDB-PG libs before any other libs to let ldd take it first
  export LD_LIBRARY_PATH=${target_dir}/lib:$LD_LIBRARY_PATH:/usr/lib

  # dependency list of all binaries and shared objects
  for f in $liblist $exelist; do
    ldd $f | awk '/=>/ {
      if (X$3 != "X" && $3 !~ /libNoVersion.so/ && $3 !~ /4[um]lib.so/ && $3 !~ /libredhat-kernel.so/ && $3 !~ /libselinux.so/ && $3 !~ /\/u01\/polardb_pg/ && $3 !~ /libjvm.so/ && $3 ~ /\.so/) {
        # printf "$s => $s\n", $1, $3
        print $3
      }
    }' >> mytmpfilelist
  done

  # deduplicate
  cat mytmpfilelist | sort -u > mytmpfilelist2

  for f in `cat mytmpfilelist2`; do
    ldd $f | awk '/=>/ {
      if (X$3 != "X" && $3 !~ /libNoVersion.so/ && $3 !~ /4[um]lib.so/ && $3 !~ /libredhat-kernel.so/ && $3 !~ /libselinux.so/ && $3 !~ /\/u01\/polardb_pg/ && $3 !~ /libjvm.so/ && $3 ~ /\.so/) {
        # printf "$s => $s\n", $1, $3
        print $3
      }
    }' >> mytmpfilelist
  done

  # deduplicate
  cat mytmpfilelist | sort -u > mytmpfilelist2

  # copy libraries if necessary
  for line in `cat mytmpfilelist2`; do
    base=`basename $line`
    dirpath=${target_dir}/lib
    filepath=$dirpath/$base

    objdump -p $line | awk 'BEGIN { START=0; LIBNAME=""; }
      /^$/ { START=0; }
      /^Dynamic Section:$/ { START=1; }
      (START==1) && /NEEDED/ {
          print $2 ;
      }
      (START==2) && /^[A-Za-z]/ { START=3; }
      /^Version References:$/ { START=2; }
      (START==2) && /required from/ {
          sub(/:/, "", $3);
          LIBNAME=$3;
      }
      (START==2) && (LIBNAME!="") && ($4!="") && (($4~/^GLIBC_*/) || ($4~/^GCC_*/)) {
          print LIBNAME "(" $4 ")";
      }
      END { exit 0 }
      ' > objdumpfile

    has_private=
    if grep -q PRIVATE objdumpfile; then
      has_private=true
    fi

    if [[ ! -f $filepath ]]; then
      if [[ $has_private != "true" ]]; then
          cp $line $dirpath
          echo $line $dirpath
      fi
    fi
  done

  rm mytmpfilelist mytmpfilelist2 objdumpfile
}

# install package dependencies
polar_install_dependency $(pwd)/${pkgname}${prefix}
cd ${buildroot}

dpkg --build ./${pkgname}
