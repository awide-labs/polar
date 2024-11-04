/*-------------------------------------------------------------------------
 *
 * polar_bufferio.c
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
 *	  src/polar_vfs/polar_bufferio.c
 *
 *-------------------------------------------------------------------------
 */
#include <sys/mman.h>

#include "polar_vfs/polar_bufferio.h"
#include "port/pg_iovec.h"
/*
 * Local file system interface.
 * It use original file system interface.
 */
const vfs_mgr polar_vfs_bio =
{
	.vfs_env_init = NULL,
	.vfs_env_destroy = NULL,
	.vfs_mount = NULL,
	.vfs_remount = NULL,
	.vfs_umount = NULL,
	.vfs_open = (vfs_open_type) open,
	.vfs_creat = creat,
	.vfs_close = close,
	.vfs_read = read,
	.vfs_write = write,
	.vfs_pread = pread,
	.vfs_preadv = pg_preadv,
	.vfs_pwrite = pwrite,
	.vfs_pwritev = pg_pwritev,
	.vfs_stat = stat,
	.vfs_fstat = fstat,
	.vfs_lstat = lstat,
	.vfs_lseek = lseek,
	.vfs_access = access,
#ifndef FRONTEND
	.vfs_fsync = pg_fsync,
#else
	.vfs_fsync = fsync,
#endif
	.vfs_unlink = unlink,
	.vfs_rename = rename,
	.vfs_posix_fallocate = posix_fallocate,
#ifdef __linux__
	.vfs_fallocate = fallocate,
#else
	.vfs_fallocate = NULL,
#endif
	.vfs_ftruncate = ftruncate,
	.vfs_truncate = truncate,
	.vfs_opendir = opendir,
	.vfs_readdir = readdir,
	.vfs_closedir = closedir,
	.vfs_mkdir = mkdir,
	.vfs_rmdir = rmdir,
	.vfs_mgr_func = NULL,
	.vfs_chmod = chmod,
	.vfs_mmap = mmap,
	.vfs_type = polar_bufferio_vfs_type,
};
