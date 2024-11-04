/*-------------------------------------------------------------------------
 *
 * polar_vfs.c
 *
 * Copyright (c) 2021, Alibaba Group Holding Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IDENTIFICATION
 *	  src/polar_vfs/polar_vfs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/ipc.h"
#include "pgstat.h"
#include "utils/guc.h"

#include "polar_vfs/polar_vfs_interface.h"

PG_MODULE_MAGIC;

Datum		polar_vfs_mem_status(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(polar_vfs_mem_status);

Datum		polar_vfs_disk_expansion(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(polar_vfs_disk_expansion);

Datum		polar_libpfs_version(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(polar_libpfs_version);

void		_PG_init(void);
void		_PG_fini(void);

static void polar_vfs_init(void);
static bool polar_in_replica_mode_or_force_mount(bool *force_mount);
static void polar_vfs_file_handle_node_type(const char *path, vfs_vfd *vfdp, polar_vfs_ops ops);
static void polar_vfs_io_handle_node_type(vfs_vfd *vfdp, ssize_t ret, polar_vfs_ops ops);
static void polar_register_vfs_fun_hooks(void);

static polar_vfs_file_hook_type polar_vfs_file_before_hook_prev = NULL;
static polar_vfs_io_hook_type polar_vfs_io_before_hook_prev = NULL;

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(WARNING, "polar_vfs init in subbackend %d", (int) getpid());
		return;
	}

	DefineCustomBoolVariable("polar_vfs.pfs_force_mount",
							 "pfs force mount mode when ro switch rw",
							 NULL,
							 &pfs_force_mount,
							 true,
							 PGC_POSTMASTER,
							 POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_UNCHANGABLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("polar_vfs.localfs_mode",
							 "enter localfs mode",
							 NULL,
							 &localfs_mode,
							 false,
							 PGC_POSTMASTER,
							 POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_UNCHANGABLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("polar_vfs.max_pfsd_io_size",
							"max pfsd io size",
							NULL,
							&max_pfsd_io_size,
							PFSD_DEFAULT_MAX_IOSIZE,
							PFSD_MIN_MAX_IOSIZE,
							PFSD_MAX_MAX_IOSIZE,
							PGC_POSTMASTER,
							POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_UNCHANGABLE,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("polar_vfs.max_direct_io_size",
							"max direct io size",
							NULL,
							&polar_max_direct_io_size,
							POLAR_DIRECTIO_DEFAULT_IOSIZE,
							POLAR_DIRECTIO_MIN_IOSIZE,
							POLAR_DIRECTIO_MAX_IOSIZE,
							PGC_POSTMASTER,
							POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_UNCHANGABLE,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("polar_vfs.debug",
							 "turn on debug switch or not",
							 NULL,
							 &polar_vfs_debug,
							 false,
							 PGC_SIGHUP,
							 POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_UNCHANGABLE,
							 NULL,
							 NULL,
							 NULL);

	EmitWarningsOnPlaceholders("polar_vfs");
	elog(LOG, "polar_vfs loaded in postmaster %d", (int) getpid());

	polar_init_vfs_cache();
	polar_init_vfs_function();
	polar_vfs_init();
	polar_register_vfs_fun_hooks();
	elog(LOG, "polar_vfs init done");
}


Datum
polar_vfs_disk_expansion(PG_FUNCTION_ARGS)
{
	char	   *expansion_disk_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (strcmp(expansion_disk_name, polar_disk_name) != 0)
		elog(ERROR, "expansion_disk_name %s is not equal with polar_disk_name %s, id %d",
			 expansion_disk_name, polar_disk_name, polar_hostid);

	if (polar_pfsd_mount_growfs(expansion_disk_name) < 0)
		elog(ERROR, "can't growfs PBD %s, id %d", expansion_disk_name, polar_hostid);

	PG_RETURN_BOOL(true);
}

Datum
polar_vfs_mem_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext oldcontext;
	HeapTuple	tuple;
	Datum		values[5];
	bool		isnull[5];
	int64		total = 0;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = NULL;
	rsinfo->setDesc = NULL;

	tupdesc = CreateTemplateTupleDesc(5);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "mem_type",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "malloc_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "malloc_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "free_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "free_bytes",
					   INT8OID, -1, 0);

	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	memset(isnull, false, sizeof(isnull));
	memset(values, 0, sizeof(values));
	values[0] = PointerGetDatum(cstring_to_text("total"));
	values[1] = Int64GetDatum(0);
	values[2] = Int64GetDatum(total);
	values[3] = Int64GetDatum(0);
	values[4] = Int64GetDatum(0);
	tuple = heap_form_tuple(tupdesc, values, isnull);
	tuplestore_puttuple(tupstore, tuple);

	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * Init polar file system
 * create shared memory and mount file system
 */
static void
polar_vfs_init(void)
{
	bool		do_force_mount = false;
	char	   *mode;
	int			flag;
	vfs_mount_arg_t polar_mount_arg;
	pfsd_mount_arg_t pfsd_mount_arg;

	if (!polar_enable_shared_storage_mode)
		return;

	if (polar_in_replica_mode_or_force_mount(&do_force_mount))
	{
		mode = "readonly";
		flag = POLAR_VFS_RD;
	}
	else
	{
		mode = "readwrite";
		flag = POLAR_VFS_RDWR;
	}

	elog(LOG, "Database will be in %s mode", mode);

	if (localfs_mode)
	{
		if (!POLAR_DIRECTIO_IS_ALIGNED(polar_max_direct_io_size))
			elog(FATAL, "polar_max_direct_io_size is not aligned!");
		else if (polar_directio_buffer == NULL &&
				 posix_memalign((void **) &polar_directio_buffer,
								PG_IO_ALIGN_SIZE,
								polar_max_direct_io_size) != 0)
		{
			elog(ERROR, "posix_memalign alloc polar_directio_buffer failed!");
		}
		else
		{
			elog(LOG, "pfs in localfs mode");
			polar_vfs_switch = POLAR_VFS_SWITCH_PLUGIN;
		}

		/*
		 * Although the local disk can determine whether the files under this
		 * path are opened in DIO mode through polar_datadir, when it comes to
		 * a specific file, it is only when the open operation is actually
		 * executed and the file path is passed in that it can be conclusively
		 * determined whether the file is opened in DIO mode.
		 */
		if (polar_datadir != NULL &&
			strncmp(POLAR_VFS_PROTOCOL_LOCAL_DIO, polar_datadir, strlen(POLAR_VFS_PROTOCOL_LOCAL_DIO)) == 0)
			polar_vfs_is_dio_mode = true;

		return;
	}

	if (do_force_mount)
	{
		flag |= POLAR_VFS_PAXOS_BYFORCE;
		mode = "readwrite (force)";
	}

	if (polar_disk_name == NULL ||
		polar_hostid <= 0 ||
		pg_strcasecmp(polar_disk_name, "") == 0 ||
		strlen(polar_disk_name) < 1)
	{
		elog(ERROR, "invalid polar_disk_name or polar_hostid");
	}

	if (polar_storage_cluster_name)
		elog(LOG, "init pangu cluster %s", polar_storage_cluster_name);

	/* mount should use pfsd_mount interface, so switch vfs kind */
	polar_vfs_switch = POLAR_VFS_SWITCH_PLUGIN;

	PFSD_INIT_MOUNT_ARG(pfsd_mount_arg, polar_storage_cluster_name, polar_disk_name, polar_hostid, flag);
	VFS_INIT_MOUNT_ARG(polar_mount_arg, polar_datadir, &pfsd_mount_arg);

	elog(LOG, "begin mount pfs name %s id %d pid %d backendid %d",
		 polar_disk_name, polar_hostid, MyProcPid, MyBackendId);
	if (polar_mount(&polar_mount_arg) != 0)
		elog(ERROR, "can't mount PBD %s, id %d with flag 0x%x",
			 polar_disk_name, polar_hostid, flag);

	polar_local_vfs_state = flag;
	polar_vfs_is_dio_mode = true;
	elog(LOG, "mount pfs %s %s mode success", polar_disk_name, mode);
}

/* When read polar_replica = on, we mount pfs use read only mode */
static bool
polar_in_replica_mode_or_force_mount(bool *force_mount)
{
	Assert(polar_local_node_type != POLAR_UNKNOWN);

	/*
	 * ro switch to rw found recovery.done, means we need do force mount
	 */
	if ((polar_local_node_type == POLAR_PRIMARY) && pfs_force_mount && force_mount)
		*force_mount = true;

	return polar_local_node_type == POLAR_REPLICA;
}

Datum
polar_libpfs_version(PG_FUNCTION_ARGS)
{
	int64		version_num = polar_pfsd_meta_version_get();
	const char *version_str = polar_pfsd_build_version_get();
	char		libpfs_version[MAXPGPATH] = {0};

	snprintf(libpfs_version, MAXPGPATH - 1, "%s version number " INT64_FORMAT "", version_str, version_num);
	PG_RETURN_TEXT_P(cstring_to_text(libpfs_version));
}

static void
polar_vfs_file_handle_node_type(const char *path, vfs_vfd *vfdp, polar_vfs_ops ops)
{
	if (polar_vfs_file_before_hook_prev)
		polar_vfs_file_before_hook_prev(path, vfdp, ops);

	switch (ops)
	{
		case VFS_CREAT:
		case VFS_UNLINK:
		case VFS_TRUNCATE:
		case VFS_MKDIR:
		case VFS_RMDIR:
		case VFS_RENAME:
			if (vfdp->kind == POLAR_VFS_LOCAL_DIO)
			{
				if (polar_is_replica() && !polar_vfs_is_writable())
					ereport(PANIC,
							errmsg("replica can't modify file in shared storage by dio: %s",
								   vfdp->file_name));
			}
			else if (vfdp->kind == POLAR_VFS_PFS)
			{
				if (polar_is_replica() && !polar_vfs_is_writable())
					ereport(WARNING,
							errmsg("replica can't modify file in shared storage by pfs: %s",
								   vfdp->file_name));
			}
			break;
		default:
			break;
	}
}

static void
polar_vfs_io_handle_node_type(vfs_vfd *vfdp, ssize_t ret, polar_vfs_ops ops)
{
	if (polar_vfs_io_before_hook_prev)
		polar_vfs_io_before_hook_prev(vfdp, ret, ops);

	switch (ops)
	{
		case VFS_WRITE:
		case VFS_PWRITE:
		case VFS_FSYNC:
		case VFS_FALLOCATE:
		case VFS_PWRITEV:
		case VFS_FTRUNCATE:
			if (vfdp->kind == POLAR_VFS_LOCAL_DIO)
			{
				if (polar_is_replica() && !polar_vfs_is_writable())
					ereport(PANIC,
							errmsg("replica can't modify file in shared storage by dio: %s",
								   vfdp->file_name));
			}
			else if (vfdp->kind == POLAR_VFS_PFS)
			{
				if (polar_is_replica() && !polar_vfs_is_writable())
					ereport(WARNING,
							errmsg("replica can't modify file in shared storage by pfs: %s",
								   vfdp->file_name));
			}
			break;
		default:
			break;
	}
}

/*
 * There is no need to unregister. Because it will do IO,
 * when proc exit.
 */
static void
polar_register_vfs_fun_hooks(void)
{
	polar_vfs_file_before_hook_prev = polar_vfs_file_before_hook;
	polar_vfs_file_before_hook = polar_vfs_file_handle_node_type;
	polar_vfs_io_before_hook_prev = polar_vfs_io_before_hook;
	polar_vfs_io_before_hook = polar_vfs_io_handle_node_type;
}
