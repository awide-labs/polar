#!/bin/bash
# polar-initdb.sh
#	  Implementation of initdb primary or replica
#
# Copyright (c) 2022, Alibaba Group Holding Limited
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
#	  src/bin/initdb/polar-initdb.sh


# arg1 is local_data
# arg2 is polar_data
# arg3 is node_type, can be primary/replica
# arg4 is cluster_name or flag "localfs" to use localfs mode
# local_data and polar_data must be full path

pfs=/usr/local/bin/pfs

local_data=$1
polar_data=$2
node_type=$3
cluster_name=$4

function check_dir()
{
	path=$1
	begin=`echo ${path:0:1}`
	end=`echo ${path:0-1}`

	if [ "$begin" != '/' ]; then
		echo "${path} must be the full path"
		exit 1
	fi

	if [ "$end" != '/' ]; then
		echo "${path} must end with /"
		exit 1
	fi
}

function cp_dir()
{
	src_path=$1
	dst_path=$2
	cluster_name=$3

	if [ -z "${src_path}" ] || [ -z "${dst_path}" ]; then
		echo "cp: src_path and dst_path can not be empty"
		exit 1
	fi

	cp_args=""
	if [ ! -z "${cluster_name}" ] && [ $cluster_name != "localfs" ]; then
		cp_args="${cp_args} -D ${cluster_name}"
	fi

	${pfs} cp ${cp_args} -r ${src_path} ${dst_path}/
	if [ $? -ne 0 ]; then
		echo "copy ${src_path} to ${dst_path}/ fail"
		exit 1
	fi
}

function rm_dir()
{
	path=$1
	if [ -z "${path}" ]; then
		echo "path can not be empty"
		exit 1
	fi

	echo "delete ${path}"
	rm -rf ${path}
	if [ $? -ne 0 ]; then
		echo "delete ${path} fail"
		exit 1
	fi
}

if [[ $node_type != @(primary|replica) ]]; then
	echo "invalid node type $node_type"
	exit 1
fi
echo "init $node_type"

if [ "$cluster_name" == "localfs" ]; then
	echo "use localfs mode"
elif [ ! -z $cluster_name ]; then
	echo "use pfs cluster $cluster_name"
	pfs_common_args="-C $cluster_name -t 20 "
fi

#check local_data and polar_data
check_dir $local_data
check_dir $polar_data

# check pfs commamd line in pfs mode
if [ "$cluster_name" == "localfs" ]; then
	pfs=""
elif [ ! -f "${pfs}" ]; then
	echo "pfs does not exist"
	exit 1
fi

# check local_data
if [ ! -d "${local_data}" ]; then
	echo "local data :("${local_data}") dir does not exist"
	exit 1
elif [ "$node_type" == "replica" ]; then
	if [ -f "${local_data}/postgresql.conf" ]; then
		echo "postgresql.conf in local exist in local dir"
		exit 1
	fi
else
	if [ ! -f "${local_data}/postgresql.conf" ]; then
		echo "postgresql.conf in local data dir does not exist"
		exit 1
	fi
 
	if [ ! -d "${local_data}/pg_twophase" ]; then
		echo "pg_twophase in local data dir does not exist"
		exit 1
	fi

	if [ ! -d "${local_data}/pg_wal" ]; then
		echo "pg_wal in local data dir does not exist"
		exit 1
	fi

	if [ ! -d "${local_data}/pg_logindex" ]; then
		echo "pg_logindex in local data dir does not exist"
		exit 1
	fi

	if [ ! -f "${local_data}/global/pg_control" ]; then
		echo "pg_control in local data dir does not exist"
		exit 1
	fi

	if [ ! -d "${local_data}/pg_xact" ]; then
		echo "pg_xact in local data dir does not exist"
		exit 1
	fi

fi

#check polar_data in pfs mode
if [ "$cluster_name" != "localfs" ]; then
	`${pfs} ${pfs_common_args} stat ${polar_data} 1>/dev/null 2>&1`
	if [ $? -ne 0 ]; then
		echo "${polar_data} dir does not exist"
		exit 1
	fi

	polar_object_type_str=`${pfs} ${pfs_common_args} stat ${polar_data}`
	polar_object_type=`expr substr "${polar_object_type_str}" 1 6`
	if [ "$polar_object_type" != '   dir' ]; then
		echo "${polar_data} is not dir"
		exit 1
	fi

	if [ "$node_type" == "primary" ]; then
		filenum_polar_data=`${pfs} ${pfs_common_args} ls ${polar_data} | wc -l`
		if [[ ${filenum_polar_data} -ne 1 ]]; then
			echo "${polar_data} dir not empty"
			exit 1
		fi
	fi
else
	`stat ${polar_data} 1>/dev/null 2>&1`
	if [ $? -ne 0 ]; then
		echo "${polar_data} dir does not exist"
		exit 1
	elif [ ! -d "${polar_data}" ]; then
		echo "${polar_data} is not dir"
		exit 1
	else
		if [ "$node_type" == "primary" ]; then
			filenum_polar_data=`ls ${polar_data} | wc -l`
			if [[ ${filenum_polar_data} -ne 0 ]]; then
				echo "${polar_data} dir not empty"
				exit 1
			fi
		fi
	fi
fi

if [ "$node_type" == "primary" ]; then
	# copy polar_settings.conf to global to make polar_settings.conf in shared storage
	mv -f ${local_data}/polar_settings.conf ${local_data}/global/polar_settings.conf
	# do copy work
	cp_dir ${local_data}/base ${polar_data} ${cluster_name}
	cp_dir ${local_data}/global ${polar_data} ${cluster_name}
	cp_dir ${local_data}/pg_tblspc ${polar_data} ${cluster_name}
	cp_dir ${local_data}/pg_wal ${polar_data} ${cluster_name}
	cp_dir ${local_data}/pg_logindex ${polar_data} ${cluster_name}
	cp_dir ${local_data}/pg_twophase ${polar_data} ${cluster_name}
	cp_dir ${local_data}/pg_xact ${polar_data} ${cluster_name}
	cp_dir ${local_data}/pg_commit_ts ${polar_data} ${cluster_name}
	cp_dir ${local_data}/pg_multixact ${polar_data} ${cluster_name}

	# do delete data
	dir_list=`ls ${local_data}/base/ 2>/dev/null`

	echo "list base dir file $dir_list"
	for dir in $dir_list
	do
		rm_dir ${local_data}base/${dir}/\*
	done
	rm_dir ${local_data}global/\*
	rm_dir ${local_data}pg_wal
	rm_dir ${local_data}pg_logindex
	rm_dir ${local_data}pg_twophase
	rm_dir ${local_data}pg_xact
	rm_dir ${local_data}pg_commit_ts
	rm_dir ${local_data}pg_multixact

	if [ ! -d "${local_data}pg_tblspc" ]; then
		echo "${polar_data}pg_tblspc not a folder"
		exit 1
	fi

	echo "init PolarDB data dir success"
elif [ "$node_type" == "replica" ]; then
	# do mkdir dir in data
	mkdir -m 700 -p ${local_data}base
	mkdir -m 700 -p ${local_data}global
	mkdir -m 700 -p ${local_data}pg_dynshmem
	mkdir -m 700 -p ${local_data}pg_log
	mkdir -m 700 -p ${local_data}pg_logical
	mkdir -m 700 -p ${local_data}pg_logical/mappings
	mkdir -m 700 -p ${local_data}pg_logical/snapshots
	mkdir -m 700 -p ${local_data}pg_notify
	mkdir -m 700 -p ${local_data}pg_replslot
	mkdir -m 700 -p ${local_data}pg_serial
	mkdir -m 700 -p ${local_data}pg_snapshots
	mkdir -m 700 -p ${local_data}pg_stat
	mkdir -m 700 -p ${local_data}pg_stat_tmp
	mkdir -m 700 -p ${local_data}pg_subtrans
	mkdir -m 700 -p ${local_data}pg_tblspc
	mkdir -m 700 -p ${local_data}pg_twophase
	mkdir -m 700 -p ${local_data}pg_commit_ts
	mkdir -m 700 -p ${local_data}pg_multixact
	mkdir -m 700 -p ${local_data}pg_multixact/members
	mkdir -m 700 -p ${local_data}pg_multixact/offsets
	mkdir -m 700 -p ${local_data}pg_xact

	echo "15" > ${local_data}PG_VERSION

	if [ ! -d "${local_data}base" ]; then
		echo "local data :("${local_data}base") dir does not exist"
		exit 1
	fi

	if [ ! -d "${local_data}pg_twophase" ]; then
		echo "local data :("${local_data}pg_twophase") dir does not exist"
		exit 1
	fi

	if [ ! -f "${local_data}PG_VERSION" ]; then
		echo "local data :("${local_data}PG_VERSION") does not exist"
		exit 1
	fi

	chmod 600 ${local_data}PG_VERSION

	# do mkdir dir in base
	dir_list=`${pfs} ${pfs_common_args} ls ${polar_data}/base/ | awk '{print $9}'`

	#base dir
	echo "list base dir file $dir_list"
	for dir in $dir_list
	do
		echo "mkdir ${local_data}base/${dir}"  
		mkdir -p ${local_data}base/${dir}
		if [ $? -ne 0 ]; then
			echo "mkdir ${local_data}base/${dir} fail"
			exit 1
		fi
	done

	echo "init PolarDB replica mode dir success"
fi

exit 0
