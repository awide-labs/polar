/*-------------------------------------------------------------------------
 *
 * polar_fd.c
 *	  PolarDB Virtual file descriptor code.
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
 *	  src/backend/storage/file/polar_fd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>		/* for getrlimit */
#endif

#include "miscadmin.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlog.h"
#include "catalog/pg_tablespace.h"
#include "common/file_perm.h"
#include "pgstat.h"
#include "portability/mem.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/resowner_private.h"
#include "storage/polar_fd.h"

#define		POLAR_REPLICA_MODE 	0x01
#define		POLAR_STANDBY_MODE	0x02
#define		POLAR_DATAMAX_MODE	0x04

int			polar_vfs_switch = POLAR_VFS_SWITCH_LOCAL;
bool		polar_vfs_is_dio_mode = false;

/*
 * Record the initial node type, it is initialized by postmaster, inherited by
 * other backends. We record this value in shared memory later.
 */
PolarNodeType polar_local_node_type = POLAR_UNKNOWN;
uint32		polar_local_vfs_state = POLAR_VFS_UNKNOWN;
static bool polar_tls_callback_registered = false;

static void polar_vfs_exit_cleanup(int code, Datum arg);

/*
 * The virtual file interface
 * 1 The standard file interface function is used by default.
 * polar_vfs_switch = POLAR_VFS_SWITCH_LOCAL
 * 2 When a storage plugin is accessed, all io call implemented in the plugin.
 * polar_vfs_switch = POLAR_VFS_SWITCH_PLUGIN
 * 3 polar_vfs means use function in plugin to read write data which include local file or
 * stared storage file.
 */
vfs_mgr		polar_vfs[] =
{
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
		.vfs_preadv = preadv,
		.vfs_pwrite = pwrite,
		.vfs_pwritev = pwritev,
		.vfs_stat = stat,
		.vfs_fstat = fstat,
		.vfs_lstat = lstat,
		.vfs_lseek = lseek,
		.vfs_access = access,
		.vfs_fsync = pg_fsync,
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
		.vfs_mgr_func = polar_get_local_vfs_mgr,
		.vfs_chmod = chmod,
		.vfs_mmap = mmap,
#if defined(HAVE_SYNC_FILE_RANGE)
		.vfs_sync_file_range = sync_file_range,
#else
		.vfs_sync_file_range = NULL,
#endif
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
		.vfs_posix_fadvise = posix_fadvise,
#else
		.vfs_posix_fadvise = NULL,
#endif
		.vfs_type = polar_bufferio_vfs_type,
	},
	{
		.vfs_env_init = NULL,
		.vfs_env_destroy = NULL,
		.vfs_mount = NULL,
		.vfs_remount = NULL,
		.vfs_umount = NULL,
		.vfs_open = NULL,
		.vfs_creat = NULL,
		.vfs_close = NULL,
		.vfs_read = NULL,
		.vfs_write = NULL,
		.vfs_pread = NULL,
		.vfs_preadv = NULL,
		.vfs_pwrite = NULL,
		.vfs_pwritev = NULL,
		.vfs_stat = NULL,
		.vfs_fstat = NULL,
		.vfs_lstat = NULL,
		.vfs_lseek = NULL,
		.vfs_access = NULL,
		.vfs_fsync = NULL,
		.vfs_unlink = NULL,
		.vfs_rename = NULL,
		.vfs_posix_fallocate = NULL,
		.vfs_fallocate = NULL,
		.vfs_ftruncate = NULL,
		.vfs_truncate = NULL,
		.vfs_opendir = NULL,
		.vfs_readdir = NULL,
		.vfs_closedir = NULL,
		.vfs_mkdir = NULL,
		.vfs_rmdir = NULL,
		.vfs_mgr_func = NULL,
		.vfs_chmod = NULL,
		.vfs_sync_file_range = NULL,
		.vfs_posix_fadvise = NULL,
		.vfs_mmap = NULL,
		.vfs_type = NULL,
	}
};

void
polar_copydir(char *fromdir, char *todir, bool recurse, bool clean, bool skip_file_err)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fromfile[MAXPGPATH * 2];
	char		tofile[MAXPGPATH * 2];
	int			read_dir_err;

	if (MakePGDirectory(todir) != 0)
	{
		if (EEXIST == errno && clean)
			polar_remove_file_in_dir(todir);
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m", todir)));
	}

	/*
	 * For polar store, the readdir will cache a dir entry, if the dir is
	 * deleted when readdir, it will fail. So we should retry.
	 *
	 * For create/drop database, we will have a lock during the copying time,
	 * no directories will be deleted, so do not care this failure.
	 */
read_dir_failed:

	read_dir_err = 0;
	xldir = AllocateDir(fromdir);
	if (xldir == NULL)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", fromdir)));
	}

	while ((xlde = polar_read_dir_ext(xldir, fromdir, WARNING, &read_dir_err)) != NULL)
	{
		struct stat fst;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(fromfile, sizeof(fromfile), "%s/%s", fromdir, xlde->d_name);
		snprintf(tofile, sizeof(tofile), "%s/%s", todir, xlde->d_name);

		if (polar_stat(fromfile, &fst) < 0)
		{
			/* File may be deleted after ReadDir */
			if (ENOENT == errno && skip_file_err)
			{
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("file \"%s\" not exist: %m", fromfile)));

				/* Skip it, continue to deal with other directories */
				continue;
			}
			else
			{
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", fromfile)));
			}
		}

		if (S_ISDIR(fst.st_mode))
		{
			if (recurse)
				polar_copydir(fromfile, tofile, true, clean, skip_file_err);
		}
		else if (S_ISREG(fst.st_mode))
			polar_copy_file(fromfile, tofile, skip_file_err);
	}
	FreeDir(xldir);

	if (read_dir_err == ENOENT)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("When readdir, some entries were deleted, retry.")));
		goto read_dir_failed;
	}

	return;
}

int
polar_copy_file(char *fromfile, char *tofile, bool skiperr)
{
	char	   *buffer;
	int			srcfd;
	int			dstfd;
	int			nbytes;
	off_t		offset;
	int			res = 0;

#define COPY_BUF_SIZE (8 * BLCKSZ)

	buffer = palloc(COPY_BUF_SIZE);

	srcfd = OpenTransientFile(fromfile, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
	{
		/* File may be deleted, skip it and free buffer */
		res = errno;
		if (ENOENT == errno && skiperr)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("file \"%s\" not exist: %m", fromfile)));
			pfree(buffer);
		}
		else
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", fromfile)));
		}
		return res;
	}

	dstfd = OpenTransientFile(tofile, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (dstfd < 0)
	{
		if (EEXIST == errno)
			dstfd = OpenTransientFile(tofile, O_RDWR | O_EXCL | PG_BINARY);

		if (dstfd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", tofile)));
	}

	for (offset = 0;; offset += nbytes)
	{
		CHECK_FOR_INTERRUPTS();

		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_READ);
		if ((nbytes = polar_read(srcfd, buffer, COPY_BUF_SIZE)) < 0)
		{
			/* File was deleted during we read it. */
			res = errno;
			if (errno == ENOENT && skiperr)
				break;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m", fromfile)));
		}
		pgstat_report_wait_end();
		if (nbytes == 0)
			break;
		errno = 0;
		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_WRITE);
		if ((int) polar_write(dstfd, buffer, nbytes) != nbytes)
		{
			pgstat_report_wait_end();
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", tofile)));
		}
		pgstat_report_wait_end();

		polar_fsync(dstfd);
	}

	if (CloseTransientFile(dstfd))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tofile)));

	CloseTransientFile(srcfd);

	pfree(buffer);
	return res;
}

struct dirent *
polar_read_dir_ext(DIR *dir, const char *dirname, int elevel, int *err)
{
	struct dirent *res;

	res = ReadDirExtended(dir, dirname, elevel);
	*err = errno;
	return res;
}

/* register tls cleanup function */
void
polar_register_tls_cleanup(void)
{
	if (polar_enable_shared_storage_mode == false)
		return;

	if (polar_vfs_switch == POLAR_VFS_SWITCH_LOCAL)
		return;

	if (polar_tls_callback_registered == false)
	{
		on_proc_exit(polar_vfs_exit_cleanup, 0);
		polar_tls_callback_registered = true;
	}
}

static void
polar_vfs_exit_cleanup(int code, Datum arg)
{
	polar_env_destroy();
	return;
}

void
polar_validate_dir(char *path)
{
	struct stat st;

	if (polar_stat(path, &st) == 0)
	{
		if (!S_ISDIR(st.st_mode))
			ereport(FATAL,
					(errmsg("required directory \"%s\" exists, but is not dir",
							path)));
	}
	else
	{
		ereport(LOG,
				(errmsg("creating missing directory \"%s\"",
						path)));

		if (MakePGDirectory(path) < 0)
			ereport(FATAL,
					(errmsg("could not create missing directory \"%s\": %m",
							path)));
	}
}

ssize_t
polar_read_line(int fd, void *buffer, size_t n)
{
	ssize_t		num_read;		/* # of bytes fetched by last read() */
	size_t		tot_read;		/* Total bytes read so far */
	char	   *buf;
	char		ch;

	if (n <= 0 || buffer == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	buf = buffer;				/* No pointer arithmetic on "void *" */

	tot_read = 0;
	for (;;)
	{
		num_read = polar_read(fd, &ch, 1);

		if (num_read == -1)
		{
			if (errno == EINTR) /* Interrupted --> restart read() */
				continue;
			else
				return -1;		/* Some other error */
		}
		else if (num_read == 0)
		{						/* EOF */
			if (tot_read == 0)	/* No bytes read; return 0 */
				return 0;
			else				/* Some bytes read; add '\0' */
				break;
		}
		else
		{						/* 'numRead' must be 1 if we get here */
			if (tot_read < n - 1)
			{					/* Discard > (n - 1) bytes */
				tot_read++;
				*buf++ = ch;
			}

			if (ch == '\n')
				break;
		}
	}

	*buf = '\0';
	return tot_read;
}

void
polar_init_node_type(void)
{
	polar_local_node_type = polar_get_node_type_by_file();
}

PolarNodeType
polar_get_node_type_by_file(void)
{
	/* use flag to show whether replica/standby is requested */
	uint8_t		flag = 0;
	struct stat stat_buf;
	PolarNodeType polar_node_type = POLAR_UNKNOWN;
	char		standby_signal_file[MAXPGPATH * 2];
	char		replica_signal_file[MAXPGPATH * 2];

	Assert(DataDir);

	/*
	 * At this time, the working path may not be changed to DataDir, so use an
	 * absolute path instead of a relative path.
	 */
	snprintf(standby_signal_file,
			 sizeof(standby_signal_file),
			 "%s/%s",
			 DataDir,
			 STANDBY_SIGNAL_FILE);

	snprintf(replica_signal_file,
			 sizeof(replica_signal_file),
			 "%s/%s",
			 DataDir,
			 REPLICA_SIGNAL_FILE);

	if (stat(standby_signal_file, &stat_buf) == 0)
		flag |= POLAR_STANDBY_MODE;

	if (stat(replica_signal_file, &stat_buf) == 0)
		flag |= POLAR_REPLICA_MODE;

	if (((flag & POLAR_REPLICA_MODE) && (flag & POLAR_STANDBY_MODE)))
	{
		ereport(FATAL, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("replica, standby mode is mutually exclusive.")));
	}

	if (flag & POLAR_REPLICA_MODE)
	{
		polar_node_type = POLAR_REPLICA;
		elog(LOG,
			 "found replica.signal, PolarDB in replica mode, use readonly mode mount pfs");
	}

	if (flag & POLAR_STANDBY_MODE)
	{
		polar_node_type = POLAR_STANDBY;
		elog(LOG,
			 "found standby.signal, PolarDB in standby mode, use readwrite mode mount pfs");
	}

	/*
	 * the default is the primary node.
	 */
	if (!flag)
	{
		polar_node_type = POLAR_PRIMARY;
	}

	return polar_node_type;
}
