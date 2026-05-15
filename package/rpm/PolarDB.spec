# PolarDB.spec
#	  Spec file for building RPM package for PolarDB-PG
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
#	  package/rpm/PolarDB.spec

# compile options

%define build_date %(echo $RELEASE)
%define code_commit %(git rev-parse HEAD || echo unknown)
%define code_branch %(git rev-parse --abbrev-ref HEAD || echo unknown)
%define polar_commit %(git rev-parse --short=8 HEAD || echo unknown)
%define pg_version %(grep AC_INIT ../../configure.ac | awk -F'[][]' '{print $4}')
%define pg_major_version %(grep AC_INIT ../../configure.ac | awk -F'[][]' '{print $4}' | awk -F'.' '{print $1}')
%define polar_majorversion %{pg_version}
%define polar_minorversion %(grep '^POLAR_MINORVERSION' ../../configure.ac | cut -d '"' -f2)
%define polar_release_date %(grep '^POLAR_RELEASE_DATE' ../../configure.ac | cut -d '"' -f2)
%define polar_version %{polar_majorversion}.%{polar_minorversion}
%define polar_pg_version %{pg_version}.%{polar_minorversion}
%define _build_id_links none
%define __spec_install_pre /bin/true
%define dir_prefix /u01
%define main_prefix %{dir_prefix}/polardb_pg
%define pkg_name PolarDB
%define release_dir %{prefix}_%{polar_release_date}

Version: %{polar_version}
Release: %{polar_commit}%{?dist}
Summary: %{pkg_name}
Name: %{pkg_name}
Group: alibaba/application
License: Commercial
BuildArch: x86_64 aarch64
Prefix: %{main_prefix}_%{pg_major_version}
AutoReqProv: none

BuildRequires: polarfs

Requires: audit-libs
Requires: cyrus-sasl-lib
Requires: glibc
Requires: keyutils-libs
Requires: krb5-libs
Requires: libcap-ng
Requires: libcom_err
Requires: libeconf
Requires: libedit
Requires: libffi
Requires: libgcc
Requires: libicu
Requires: libselinux
Requires: libstdc++
Requires: libuuid
Requires: libxcrypt
Requires: libxml2
Requires: libxslt
Requires: llvm15-libs
Requires: ncurses-libs
Requires: openldap
Requires: openssl-libs
Requires: pam
Requires: pcre2
Requires: perl-libs
Requires: polarfs
Requires: python3-libs
Requires: tcl
Requires: xerces-c
Requires: xz-libs
Requires: zlib

%description
CodeBranch: %{code_branch}
CodeCommit: %{code_commit}
PolarCommit: %{polar_commit}
PolarVersion: %{polar_version}
PolarPGVersion: %{polar_pg_version}
PolarReleaseDate: %{polar_release_date}
PFSDVersion: %{pfsd_version}
PolarDB is an advanced Object-Relational database management system
(DBMS) that supports almost all SQL constructs (including
transactions, subselects and user-defined types and functions). The
PolarDB package includes the client programs and libraries that
you'll need to access a PolarDB DBMS server.  These PolarDB
client programs are programs that directly manipulate the internal
structure of PolarDB databases on a PolarDB server. These client
programs can be located on the same machine with the PolarDB
server, or may be on a remote machine which accesses a PolarDB
server over a network connection. This package contains the command-line
utilities for managing PolarDB databases on a PolarDB server.

%build

common_build_flag='--port=5432 --debug=off --jobs=16 --ni'
common_configure_flag='--with-deploy-mode=opensource --with-pfsd'

export CC=gcc CXX=g++
export NM=gcc-nm AR=gcc-ar RANLIB=gcc-ranlib

%if "%{rhel}" == "8"
common_configure_flag+=' --enable-dtrace'
%endif

# Avoid using default compiler.
%if "%{?dist}" == ".el8"
export PATH=/opt/rh/gcc-toolset-10/root/usr/bin:$PATH
%endif

# Some module may incur some unexpected compiler warnings, ignore them.
export COPT="-Wno-error ${COPT-}"

cd $OLDPWD/../../

DESTDIR=%{buildroot} \
  ./build.sh \
    --ec="--prefix=%{prefix} ${common_configure_flag} ${extra_configure_flag}" \
    ${common_build_flag}

polar_install_dependency()
{
  pkg_type=${1}
  target_dir=${2}

  # create link for lib inside lib
  cd %{buildroot}${target_dir}/lib/
  ln -sf ../lib ./lib

  cd %{buildroot}

  # generate list of .so files
  # collect all the executable binaries, scripts and shared libraries
  binfiles=`find %{buildroot}${target_dir}/bin`
  libfiles=`find %{buildroot}${target_dir}/lib`
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
  eval PERL_PATH=`cd /usr/lib64/perl[5-9]/CORE/ ; pwd`
  export LD_LIBRARY_PATH=%{buildroot}${target_dir}/lib:$PERL_PATH:$LD_LIBRARY_PATH:/usr/lib

  # dependency list of all binaries and shared objects
  for f in $liblist $exelist; do
    ldd $f | awk '/=>/ {
      if (X$3 != "X" && $3 !~ /libNoVersion.so/ && $3 !~ /4[um]lib.so/ && $3 !~ /libredhat-kernel.so/ && $3 !~ /libselinux.so/ && $3 !~ /libjvm.so/ && $3 ~ /\.so/) {
        # printf "$s => $s\n", $1, $3
        print $3
      }
    }' >> mytmpfilelist
  done

  # deduplicate
  cat mytmpfilelist | sort -u > mytmpfilelist2

  for f in `cat mytmpfilelist2`; do
    ldd $f | awk '/=>/ {
      if (X$3 != "X" && $3 !~ /libNoVersion.so/ && $3 !~ /4[um]lib.so/ && $3 !~ /libredhat-kernel.so/ && $3 !~ /libselinux.so/ && $3 !~ /libjvm.so/ && $3 ~ /\.so/) {
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
    dirpath=%{buildroot}${target_dir}/lib
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
      fi
    fi
  done

  rm mytmpfilelist mytmpfilelist2 objdumpfile

  # avoid failure of call to /usr/lib/rpm/check-buildroot
  export QA_SKIP_BUILD_ROOT=1
}

# install package dependencies
polar_install_dependency main %{prefix}

%post
# create a fork of files named with release date
if [[ -f %{release_dir}/bin/postgres ]];
then
  echo WARNING: PolarDB-PG files already exist, file copying will be skipped!
else
  if [[ -d %{release_dir} ]];
  then
    cp -rf %{prefix}/* %{release_dir}
  else
    cp -rf %{prefix} %{release_dir}
  fi
fi

%files
%defattr(-,root,root)
%{prefix}/*

%define __os_install_post \
%{nil}
