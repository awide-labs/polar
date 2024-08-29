/*-------------------------------------------------------------------------
 *
 * polar_vfs_interface.h
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
 *    src/include/polar_vfs/polar_vfs_interface.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_VFS_INTERFACE_H
#define POLAR_VFS_INTERFACE_H
#include "postgres.h"
#include <unistd.h>
#include <sys/time.h>
#include <semaphore.h>
#include <pthread.h>
#ifndef FRONTEND
#include "access/htup_details.h"
#include "access/xlogrecovery.h"
#include "funcapi.h"
#include "tcop/utility.h"
#include "storage/pg_shmem.h"
#include "postmaster/postmaster.h"
#include "utils/builtins.h"
#endif

#include "access/xlog.h"
#include "miscadmin.h"
#include "polar_vfs/polar_bufferio.h"
#include "polar_vfs/polar_directio.h"
#include "polar_vfs/polar_pfsd.h"

#define MIN_VFS_FD_DIR_SIZE 10
#define VFD_CLOSED (-1)

typedef struct vfs_vfd
{
	int			fd;
	int			kind;
	int			type;			/* enum { data, xlog,clog } */
	int			next_free;
	char	   *file_name;
} vfs_vfd;

typedef struct
{
	int			kind;
	DIR		   *dir;
} vfs_dir_desc;

typedef enum polar_vfs_ops
{
	VFS_ENV_INIT,
	VFS_ENV_DESTROY,
	VFS_MOUNT,
	VFS_REMOUNT,
	VFS_OPEN,
	VFS_CREAT,
	VFS_CLOSE,
	VFS_READ,
	VFS_WRITE,
	VFS_PREAD,
	VFS_PREADV,
	VFS_PWRITE,
	VFS_PWRITEV,
	VFS_STAT,
	VFS_FSTAT,
	VFS_LSTAT,
	VFS_LSEEK,
	VFS_LSEEK_CACHE,
	VFS_ACCESS,
	VFS_FSYNC,
	VFS_UNLINK,
	VFS_RENAME,
	VFS_FALLOCATE,
	VFS_FTRUNCATE,
	VFS_TRUNCATE,
	VFS_OPENDIR,
	VFS_READDIR,
	VFS_CLOSEDIR,
	VFS_MKDIR,
	VFS_RMDIR,
	VFS_MGR_FUNC,
	VFS_CHMOD
} polar_vfs_ops;

extern bool pfs_force_mount;
extern bool localfs_mode;
extern bool polar_vfs_debug;
extern char *polar_disk_name;
extern char *polar_storage_cluster_name;
extern int	polar_hostid;

extern void polar_init_vfs_cache(void);
extern bool polar_init_vfs_function(void);

/*
 * polar vfs subsystem hook.
 * (1) polar_vfs_env_hook_type is hook type for env related ops, such as:
 * vfs_env_init vfs_env_destroy
 * (2) polar_vfs_file_hook_type is hook type for file related ops, such as:
 * vfs_create vfs_open vfs_close
 * (3) polar_vfs_io_hook_type is hook type for I/O related ops, such as:
 * vfs_(p)write vfs_(p)read vfs_lseek vfs_fsync vfs_fallocate
 */
typedef void (*polar_vfs_env_hook_type) (polar_vfs_ops ops);
extern PGDLLIMPORT polar_vfs_env_hook_type polar_vfs_env_init_hook;
extern PGDLLIMPORT polar_vfs_env_hook_type polar_vfs_env_destroy_hook;

typedef void (*polar_vfs_file_hook_type) (const char *path, vfs_vfd *vfdp, polar_vfs_ops ops);
extern PGDLLIMPORT polar_vfs_file_hook_type polar_vfs_file_before_hook;
extern PGDLLIMPORT polar_vfs_file_hook_type polar_vfs_file_after_hook;

typedef void (*polar_vfs_io_hook_type) (vfs_vfd *vfdp, ssize_t ret, polar_vfs_ops ops);
extern PGDLLIMPORT polar_vfs_io_hook_type polar_vfs_io_before_hook;
extern PGDLLIMPORT polar_vfs_io_hook_type polar_vfs_io_after_hook;

#endif
