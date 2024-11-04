/*-------------------------------------------------------------------------
 *
 * polar_vfs_interface.c
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
 *	  src/polar_vfs/polar_vfs_interface.c
 *
 *-------------------------------------------------------------------------
 */
#include "polar_vfs/polar_vfs_interface.h"

#define POLAR_VFS_FD_MASK 0x40000000
#define POLAR_VFS_FD_MASK_ADD(fd) \
do { \
	Assert(fd < POLAR_VFS_FD_MASK); \
	fd = fd | POLAR_VFS_FD_MASK; \
} while (0)
#define POLAR_VFS_FD_MASK_RMOVE(fd) \
do { \
	Assert(fd > POLAR_VFS_FD_MASK); \
	fd = fd & ~POLAR_VFS_FD_MASK; \
} while (0)

#define CHECK_FD_REENTRANT_BEGIN() \
do { \
	Assert(!fd_reentrant); \
	fd_reentrant = true; \
} while (0)

#define CHECK_FD_REENTRANT_END() \
do { \
	fd_reentrant = false; \
} while (0)

#ifndef FRONTEND
#define VFS_HOLD_INTERRUPTS() HOLD_INTERRUPTS()
#else
#define VFS_HOLD_INTERRUPTS()
#endif

#ifndef FRONTEND
#define VFS_RESUME_INTERRUPTS() RESUME_INTERRUPTS()
#else
#define VFS_RESUME_INTERRUPTS()
#endif

static vfs_vfd *vfs_vfd_cache = NULL;
static size_t size_vfd_cache = 0;
static int	num_open_file = 0;

static int	num_vfs_dir_descs = 0;
static int	max_vfs_dir_descs = 0;
static vfs_dir_desc *vfs_dir_descs = NULL;
static bool mounted = false;
static bool fd_reentrant = false;

static int	vfs_env_init(void);
static int	vfs_env_destroy(void);
static int	vfs_mount(vfs_mount_arg_t *mount_arg);
static int	vfs_remount(vfs_mount_arg_t *remount_arg);
static int	vfs_umount(char *ftype, const char *pbdname);

static int	vfs_creat(const char *path, mode_t mode);
static int	vfs_open(const char *path, int flags, mode_t mode);
static int	vfs_close(int file);

static ssize_t vfs_read(int file, void *buf, size_t len);
static ssize_t vfs_write(int file, const void *buf, size_t len);
static ssize_t vfs_pread(int file, void *buf, size_t len, off_t offset);
static ssize_t vfs_preadv(int file, const struct iovec *iov, int iovcnt, off_t offset);
static ssize_t vfs_pwrite(int file, const void *buf, size_t len, off_t offset);
static ssize_t vfs_pwritev(int file, const struct iovec *iov, int iovcnt, off_t offset);

static int	vfs_stat(const char *path, struct stat *buf);
static int	vfs_fstat(int file, struct stat *buf);
static int	vfs_lstat(const char *path, struct stat *buf);
static off_t vfs_lseek(int file, off_t offset, int whence);
static int	vfs_access(const char *path, int mode);

static int	vfs_fsync(int file);
static int	vfs_unlink(const char *fname);
static int	vfs_rename(const char *oldfile, const char *newfile);
static int	vfs_posix_fallocate(int file, off_t offset, off_t len);
static int	vfs_fallocate(int file, int mode, off_t offset, off_t len);
static int	vfs_ftruncate(int file, off_t len);
static int	vfs_truncate(const char *path, off_t len);

static DIR *vfs_opendir(const char *dirname);
static struct dirent *vfs_readdir(DIR *dir);
static int	vfs_closedir(DIR *dir);
static int	vfs_mkdir(const char *path, mode_t mode);
static int	vfs_rmdir(const char *path);

static inline void vfs_free_vfd(int file);
static inline File vfs_allocate_vfd(void);
static inline bool vfs_allocated_dir(void);
static inline vfs_vfd *vfs_find_file(int file);
static inline int vfs_file_type(const char *path);
static const vfs_mgr *vfs_get_mgr(const char *path);
static int	vfs_chmod(const char *path, mode_t mode);
static inline const char *polar_vfs_file_type_and_path(const char *path, int *kind);
static void *vfs_mmap(void *start, size_t length, int prot, int flags, int file, off_t offset);

static PolarVFSKind vfs_type(int fd);

static const vfs_mgr *const vfs[POLAR_VFS_KIND_SIZE] =
{
	/* Local file system interface. */
	&polar_vfs_bio,
	/* Pfsd file system interface. */
	&polar_vfs_pfsd,
	/* Local file system interface with O_DIRECT flag. */
	&polar_vfs_dio
};

/*
 * polar_vfs_kind[*] must conform to a certain format:
 * [protocol]://
 * Finally, polar_datadir will look like the following format:
 * [protocol]://[path]
 *
 * Notice: If you change the format of polar_vfs_kind[*], you must
 * modify the function polar_path_remove_protocol(...) in polar_fd.c.
 */
static const char polar_vfs_kind[POLAR_VFS_KIND_SIZE][POLAR_VFS_PROTOCOL_MAX_LEN] =
{
	POLAR_VFS_PROTOCOL_LOCAL_BIO, //POLAR_VFS_LOCAL_BIO
	POLAR_VFS_PROTOCOL_PFS, //POLAR_VFS_PFS
	POLAR_VFS_PROTOCOL_LOCAL_DIO // POLAR_VFS_LOCAL_DIO
};

/*
 * POLAR:
 * For some io operations, need to prevent fd re-entry
 * in elog, therefore, HOLD INTERRUPTS is necessary.
 * Such as vfs close, after the file is closed, it may
 * be closed again in the interrupt process.
 */
static const vfs_mgr vfs_interface =
{
	.vfs_env_init = vfs_env_init,
	.vfs_env_destroy = vfs_env_destroy,
	.vfs_mount = vfs_mount,
	.vfs_remount = vfs_remount,
	.vfs_umount = vfs_umount,
	.vfs_open = vfs_open,
	.vfs_creat = vfs_creat,
	.vfs_close = vfs_close,
	.vfs_read = vfs_read,
	.vfs_write = vfs_write,
	.vfs_pread = vfs_pread,
	.vfs_preadv = vfs_preadv,
	.vfs_pwrite = vfs_pwrite,
	.vfs_pwritev = vfs_pwritev,
	.vfs_stat = vfs_stat,
	.vfs_fstat = vfs_fstat,
	.vfs_lstat = vfs_lstat,
	.vfs_lseek = vfs_lseek,
	.vfs_access = vfs_access,
	.vfs_fsync = vfs_fsync,
	.vfs_unlink = vfs_unlink,
	.vfs_rename = vfs_rename,
	.vfs_posix_fallocate = vfs_posix_fallocate,
	.vfs_fallocate = vfs_fallocate,
	.vfs_ftruncate = vfs_ftruncate,
	.vfs_truncate = vfs_truncate,
	.vfs_opendir = vfs_opendir,
	.vfs_readdir = vfs_readdir,
	.vfs_closedir = vfs_closedir,
	.vfs_mkdir = vfs_mkdir,
	.vfs_rmdir = vfs_rmdir,
	.vfs_mgr_func = vfs_get_mgr,
	.vfs_chmod = vfs_chmod,
	.vfs_mmap = vfs_mmap,
	.vfs_type = vfs_type,
};

bool		localfs_mode = false;
bool		pfs_force_mount = true;
bool		polar_vfs_debug = false;
extern int	max_safe_fds;

/* Hooks for polar vfs subsystem. */
polar_vfs_env_hook_type polar_vfs_env_init_hook = NULL;
polar_vfs_env_hook_type polar_vfs_env_destroy_hook = NULL;
polar_vfs_file_hook_type polar_vfs_file_before_hook = NULL;
polar_vfs_file_hook_type polar_vfs_file_after_hook = NULL;
polar_vfs_io_hook_type polar_vfs_io_before_hook = NULL;
polar_vfs_io_hook_type polar_vfs_io_after_hook = NULL;

/* Init local variable for file handle or dir pointer */
void
polar_init_vfs_cache(void)
{
	if (vfs_vfd_cache == NULL)
	{
		vfs_vfd_cache = (vfs_vfd *) malloc(sizeof(vfs_vfd));
		if (vfs_vfd_cache == NULL)
		{
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory vfs_vfd_cache")));
		}
		MemSet((char *) &(vfs_vfd_cache[0]), 0, sizeof(vfs_vfd));
		vfs_vfd_cache->fd = VFD_CLOSED;
		size_vfd_cache = 1;
	}
	else
	{
#ifndef FRONTEND
		elog(WARNING, "vfs_vfd_cache already initialized size %zu", size_vfd_cache);
#endif
	}

	if (vfs_dir_descs == NULL)
	{
		vfs_dir_desc *new_descs;
		int			new_max;

		new_max = MIN_VFS_FD_DIR_SIZE;
		new_descs = (vfs_dir_desc *) malloc(new_max * sizeof(vfs_dir_desc));
		if (new_descs == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory vfs_dir_desc")));
		}
		vfs_dir_descs = new_descs;
		max_vfs_dir_descs = new_max;
		num_vfs_dir_descs = 0;
	}
	else
	{
#ifndef FRONTEND
		elog(WARNING, "vfs_dir_descs already initialized size %d", num_vfs_dir_descs);
#endif
	}

	return;
}

bool
polar_init_vfs_function(void)
{
	if (polar_enable_shared_storage_mode == false)
		return false;

	if (polar_vfs[POLAR_VFS_SWITCH_PLUGIN].vfs_env_init != NULL)
	{
#ifndef FRONTEND
		elog(WARNING, "vfs some of vfs_fun pointer are not NULL");
#endif
		return false;
	}

	polar_vfs[POLAR_VFS_SWITCH_PLUGIN] = vfs_interface;

	return true;
}

static int
vfs_env_init(void)
{
	if (vfs[POLAR_VFS_PFS]->vfs_env_init &&
		localfs_mode == false)
		vfs[POLAR_VFS_PFS]->vfs_env_init();

	if (polar_vfs_env_init_hook)
		polar_vfs_env_init_hook(VFS_ENV_INIT);

	return 0;
}

static int
vfs_env_destroy(void)
{
	if (vfs[POLAR_VFS_PFS]->vfs_env_destroy &&
		polar_enable_shared_storage_mode == true)
		vfs[POLAR_VFS_PFS]->vfs_env_destroy();

	if (polar_vfs_env_destroy_hook)
		polar_vfs_env_destroy_hook(VFS_ENV_DESTROY);

	return 0;
}

static int
vfs_mount(vfs_mount_arg_t *mount_arg)
{
	int			rc = 0;
	int			kind = -1;

	polar_vfs_file_type_and_path(mount_arg->ftype, &kind);

	if (vfs[kind]->vfs_mount)
		rc = vfs[kind]->vfs_mount(mount_arg);

	if (rc == 0)
		mounted = true;

	return rc;
}

static int
vfs_remount(vfs_mount_arg_t *remount_arg)
{
	int			rc = 0;
	int			kind = -1;

	polar_vfs_file_type_and_path(remount_arg->ftype, &kind);

	if (vfs[kind]->vfs_remount)
		rc = vfs[kind]->vfs_remount(remount_arg);

	if (rc != 0)
	{
		if (!mounted)
			elog(LOG, "PBD %s not mounted, so remount failed", polar_disk_name);

		elog(ERROR, "can't remount PBD %s, id %d", polar_disk_name, polar_hostid);
	}
	else
		elog(LOG, "remount pfs %s readwrite mode success", polar_disk_name);

	return 0;
}

static int
vfs_umount(char *ftype, const char *pbdname)
{
	int			rc = 0;
	int			kind = -1;

	if (!mounted && !localfs_mode)
	{
		elog(WARNING, "pbdname %s not mount", pbdname);
		return rc;
	}

	polar_vfs_file_type_and_path(ftype, &kind);

	if (vfs[kind]->vfs_umount)
		rc = vfs[kind]->vfs_umount(ftype, pbdname);

	if (rc == 0 && !localfs_mode)
	{
		elog(LOG, "umount pbd %s success", pbdname);
		mounted = false;
	}
	return rc;
}

static int
vfs_creat(const char *path, mode_t mode)
{
	int			file = vfs_allocate_vfd();
	const char *vfs_path;
	vfs_vfd    *vfdP = &vfs_vfd_cache[file];
	int			save_errno;

	VFS_HOLD_INTERRUPTS();
	vfs_path = polar_vfs_file_type_and_path(path, &(vfdP->kind));

	if (polar_vfs_file_before_hook)
		polar_vfs_file_before_hook(path, vfdP, VFS_CREAT);

	vfdP->fd = vfs[vfdP->kind]->vfs_creat(vfs_path, mode);
	save_errno = errno;

	if (polar_vfs_file_after_hook)
		polar_vfs_file_after_hook(path, vfdP, VFS_CREAT);

	if (vfdP->fd < 0)
	{
		vfs_free_vfd(file);

		VFS_RESUME_INTERRUPTS();
		errno = save_errno;
		return -1;
	}

	Assert(vfdP->file_name == NULL);
	vfdP->file_name = strdup(path);
	num_open_file++;

	if (unlikely(polar_vfs_debug))
		elog(LOG, "vfs creat file %s, fd %d file %d num open file %d", vfdP->file_name ? vfdP->file_name : "null", vfdP->fd, file, num_open_file);
	POLAR_VFS_FD_MASK_ADD(file);
	VFS_RESUME_INTERRUPTS();
	errno = save_errno;
	return file;
}

static int
vfs_open(const char *path, int flags, mode_t mode)
{
	int			file = -1;
	vfs_vfd    *vfdP = NULL;
	const char *vfs_path;
	int			save_errno;

	if (path == NULL)
		return -1;

	VFS_HOLD_INTERRUPTS();
	file = vfs_allocate_vfd();
	vfdP = &vfs_vfd_cache[file];

	vfs_path = polar_vfs_file_type_and_path(path, &(vfdP->kind));

	if (polar_vfs_file_before_hook)
		polar_vfs_file_before_hook(path, vfdP, VFS_OPEN);

	vfdP->fd = vfs[vfdP->kind]->vfs_open(vfs_path, flags, mode);
	save_errno = errno;

	if (polar_vfs_file_after_hook)
		polar_vfs_file_after_hook(path, vfdP, VFS_OPEN);

	if (vfdP->fd < 0)
	{
		vfs_free_vfd(file);

		VFS_RESUME_INTERRUPTS();

		errno = save_errno;
		return -1;
	}
	Assert(vfdP->file_name == NULL);
	vfdP->file_name = strdup(path);
	num_open_file++;

	if (unlikely(polar_vfs_debug))
		elog(LOG, "vfs open file %s num open file %d", path, num_open_file);
	POLAR_VFS_FD_MASK_ADD(file);
	VFS_RESUME_INTERRUPTS();
	errno = save_errno;
	return file;
}

static int
vfs_close(int file)
{
	vfs_vfd    *vfdP = NULL;
	int			rc = -1;
	int			save_errno;

	VFS_HOLD_INTERRUPTS();

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (vfdP->fd != VFD_CLOSED)
	{
		rc = vfs[vfdP->kind]->vfs_close(vfdP->fd);
		save_errno = errno;

		if (rc != 0)
			elog(WARNING, "vfs could not close file \"%s\": %m", vfdP->file_name);

		if (polar_vfs_file_after_hook)
			polar_vfs_file_after_hook(NULL, vfdP, VFS_CLOSE);

		if (unlikely(polar_vfs_debug))
			elog(LOG, "vfs close file %s, fd %d file %d num open file %d", vfdP->file_name, vfdP->fd, file, num_open_file);

		--num_open_file;
		vfdP->fd = VFD_CLOSED;
		vfs_free_vfd(file);
	}
	else
	{
		elog(WARNING, "vfs file %s file %d not open", vfdP->file_name, file);

		/*
		 * POLAR: If the fd has been closed, it indicates that the fd is bad
		 * file number.
		 */
		save_errno = EBADF;
	}

	CHECK_FD_REENTRANT_END();
	VFS_RESUME_INTERRUPTS();
	errno = save_errno;
	return rc;
}

static ssize_t
vfs_write(int file, const void *buf, size_t len)
{
	vfs_vfd    *vfdP = NULL;
	ssize_t		res = -1;
	int			save_errno;

	VFS_HOLD_INTERRUPTS();

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_WRITE);

	res = vfs[vfdP->kind]->vfs_write(vfdP->fd, buf, len);
	save_errno = errno;

	if (polar_vfs_io_after_hook)
		polar_vfs_io_after_hook(vfdP, res, VFS_WRITE);

	if (unlikely(polar_vfs_debug))
		elog(LOG, "vfs_write file: %s, buf: 0x%lx, len: 0x%lx, kind: %d",
			 vfdP->file_name, (unsigned long) buf, len, vfdP->kind);

	VFS_RESUME_INTERRUPTS();

	CHECK_FD_REENTRANT_END();
	errno = save_errno;
	return res;
}

static ssize_t
vfs_read(int file, void *buf, size_t len)
{
	vfs_vfd    *vfdP = NULL;
	ssize_t		res = -1;
	int			save_errno;

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_READ);

	res = vfs[vfdP->kind]->vfs_read(vfdP->fd, buf, len);
	save_errno = errno;

	if (polar_vfs_io_after_hook)
		polar_vfs_io_after_hook(vfdP, res, VFS_READ);

	CHECK_FD_REENTRANT_END();
	errno = save_errno;
	return res;
}

static ssize_t
vfs_pread(int file, void *buf, size_t len, off_t offset)
{
	vfs_vfd    *vfdP = NULL;
	ssize_t		res = -1;
	int			save_errno;

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_PREAD);

	res = vfs[vfdP->kind]->vfs_pread(vfdP->fd, buf, len, offset);
	save_errno = errno;

	if (polar_vfs_io_after_hook)
		polar_vfs_io_after_hook(vfdP, res, VFS_PREAD);

	CHECK_FD_REENTRANT_END();
	errno = save_errno;
	return res;
}

static ssize_t
vfs_preadv(int file, const struct iovec *iov, int iovcnt, off_t offset)
{
	vfs_vfd    *vfdP = NULL;
	ssize_t		res = -1;
	int			save_errno;

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_PREADV);

	res = vfs[vfdP->kind]->vfs_preadv(vfdP->fd, iov, iovcnt, offset);
	save_errno = errno;

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_PREADV);

	CHECK_FD_REENTRANT_END();
	errno = save_errno;
	return res;
}

static ssize_t
vfs_pwrite(int file, const void *buf, size_t len, off_t offset)
{
	vfs_vfd    *vfdP = NULL;
	ssize_t		res = -1;
	int			save_errno;

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_PWRITE);

	res = vfs[vfdP->kind]->vfs_pwrite(vfdP->fd, buf, len, offset);
	save_errno = errno;

	if (polar_vfs_io_after_hook)
		polar_vfs_io_after_hook(vfdP, res, VFS_PWRITE);

	CHECK_FD_REENTRANT_END();
	errno = save_errno;
	return res;
}

static ssize_t
vfs_pwritev(int file, const struct iovec *iov, int iovcnt, off_t offset)
{
	vfs_vfd    *vfdP = NULL;
	ssize_t		res = -1;
	size_t		len = 0;
	int			i;
	int			save_errno;

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_PWRITEV);

	res = vfs[vfdP->kind]->vfs_pwritev(vfdP->fd, iov, iovcnt, offset);
	save_errno = errno;

	if (polar_vfs_io_after_hook)
		polar_vfs_io_after_hook(vfdP, res, VFS_PWRITEV);

	for (i = 0; i < iovcnt; i++)
		len += iov[i].iov_len;

	CHECK_FD_REENTRANT_END();
	errno = save_errno;
	return res;

}

static int
vfs_stat(const char *path, struct stat *buf)
{
	int			rc = -1;
	int			kind = -1;
	const char *vfs_path;

	if (path == NULL)
		return -1;

	vfs_path = polar_vfs_file_type_and_path(path, &kind);
	rc = vfs[kind]->vfs_stat(vfs_path, buf);

	return rc;
}

static int
vfs_fstat(int file, struct stat *buf)
{
	vfs_vfd    *vfdP = NULL;
	int			rc = 0;

	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);
	rc = vfs[vfdP->kind]->vfs_fstat(vfdP->fd, buf);

	return rc;
}

static int
vfs_lstat(const char *path, struct stat *buf)
{
	int			rc = -1;
	int			kind = -1;
	const char *vfs_path;

	if (path == NULL)
		return -1;

	vfs_path = polar_vfs_file_type_and_path(path, &kind);
	rc = vfs[kind]->vfs_lstat(vfs_path, buf);

	return rc;
}

static off_t
vfs_lseek(int file, off_t offset, int whence)
{
	vfs_vfd    *vfdP = NULL;
	off_t		rc = 0;
	int			save_errno;

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_LSEEK);

	rc = vfs[vfdP->kind]->vfs_lseek(vfdP->fd, offset, whence);
	save_errno = errno;

	if (polar_vfs_io_after_hook)
		polar_vfs_io_after_hook(vfdP, 0, VFS_LSEEK);

	CHECK_FD_REENTRANT_END();
	errno = save_errno;
	return rc;
}

int
vfs_access(const char *path, int mode)
{
	int			rc = -1;
	int			kind = -1;
	const char *vfs_path;

	if (path == NULL)
		return -1;

	vfs_path = polar_vfs_file_type_and_path(path, &kind);
	rc = vfs[kind]->vfs_access(vfs_path, mode);

	return rc;
}

static int
vfs_fsync(int file)
{
	vfs_vfd    *vfdP = NULL;
	int			rc = 0;
	int			save_errno;

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_FSYNC);

	rc = vfs[vfdP->kind]->vfs_fsync(vfdP->fd);
	save_errno = errno;

	if (polar_vfs_io_after_hook)
		polar_vfs_io_after_hook(vfdP, 0, VFS_FSYNC);

	CHECK_FD_REENTRANT_END();
	errno = save_errno;
	return rc;
}

static int
vfs_unlink(const char *fname)
{
	int			rc = -1;
	int			kind = -1;
	const char *vfs_path;
	vfs_vfd		vfdP;

	if (fname == NULL)
		return -1;

	VFS_HOLD_INTERRUPTS();

	elog(LOG, "vfs_unlink %s", fname);

	vfs_path = polar_vfs_file_type_and_path(fname, &kind);

	vfdP.kind = kind;
	if (polar_vfs_file_before_hook)
		polar_vfs_file_before_hook(fname, &vfdP, VFS_UNLINK);

	rc = vfs[kind]->vfs_unlink(vfs_path);

	VFS_RESUME_INTERRUPTS();

	return rc;
}

static int
vfs_rename(const char *oldfile, const char *newfile)
{
	int			rc = -1;
	int			kindold = -1;
	int			kindnew = -1;
	const char *vfs_old_path;
	const char *vfs_new_path;
	vfs_vfd		vfdP;

	if (oldfile == NULL || newfile == NULL)
		return -1;

	VFS_HOLD_INTERRUPTS();

	vfs_old_path = polar_vfs_file_type_and_path(oldfile, &kindold);
	vfs_new_path = polar_vfs_file_type_and_path(newfile, &kindnew);

	vfdP.kind = kindold;
	if (polar_vfs_file_before_hook)
	{
		polar_vfs_file_before_hook(oldfile, &vfdP, VFS_RENAME);
		polar_vfs_file_before_hook(newfile, &vfdP, VFS_RENAME);
	}

	elog(LOG, "vfs_rename from %s to %s", oldfile, newfile);

	if (kindold == kindnew)
	{
		rc = vfs[kindold]->vfs_rename(vfs_old_path, vfs_new_path);
		VFS_RESUME_INTERRUPTS();
	}
	else
	{
		VFS_RESUME_INTERRUPTS();
		elog(ERROR, "vfs unsupported rename operation");
	}

	return rc;
}

static int
vfs_posix_fallocate(int file, off_t offset, off_t len)
{
	vfs_vfd    *vfdP = NULL;
	int			rc = 0;
	int			save_errno;

	VFS_HOLD_INTERRUPTS();

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (unlikely(polar_vfs_debug))
		elog(LOG, "vfs_posix_fallocate from %s", vfdP->file_name);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_FALLOCATE);

	rc = vfs[vfdP->kind]->vfs_posix_fallocate(vfdP->fd, offset, len);
	save_errno = errno;

	if (polar_vfs_io_after_hook)
		polar_vfs_io_after_hook(vfdP, 0, VFS_FALLOCATE);

	CHECK_FD_REENTRANT_END();

	VFS_RESUME_INTERRUPTS();

	errno = save_errno;
	return rc;
}

static int
vfs_fallocate(int file, int mode, off_t offset, off_t len)
{
	vfs_vfd    *vfdP = NULL;
	int			rc = 0;
	int			save_errno;

	VFS_HOLD_INTERRUPTS();

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	if (unlikely(polar_vfs_debug))
		elog(LOG, "vfs_fallocate from %s", vfdP->file_name);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_FALLOCATE);

	rc = vfs[vfdP->kind]->vfs_fallocate(vfdP->fd, mode, offset, len);
	save_errno = errno;

	if (polar_vfs_io_after_hook)
		polar_vfs_io_after_hook(vfdP, 0, VFS_FALLOCATE);

	CHECK_FD_REENTRANT_END();

	VFS_RESUME_INTERRUPTS();

	errno = save_errno;
	return rc;
}

static int
vfs_ftruncate(int file, off_t len)
{
	vfs_vfd    *vfdP = NULL;
	int			rc = 0;

	VFS_HOLD_INTERRUPTS();

	CHECK_FD_REENTRANT_BEGIN();
	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	elog(LOG, "vfs_ftruncate from %s", vfdP->file_name);

	if (polar_vfs_io_before_hook)
		polar_vfs_io_before_hook(vfdP, 0, VFS_FTRUNCATE);

	rc = vfs[vfdP->kind]->vfs_ftruncate(vfdP->fd, len);

	CHECK_FD_REENTRANT_END();

	VFS_RESUME_INTERRUPTS();

	return rc;
}

static int
vfs_truncate(const char *path, off_t len)
{
	const char *vfs_path;
	int			kind = -1;
	int			rc = -1;
	vfs_vfd		vfdP;

	if (path == NULL)
		return -1;

	VFS_HOLD_INTERRUPTS();

	elog(LOG, "vfs_truncate %s", path);

	vfs_path = polar_vfs_file_type_and_path(path, &kind);

	vfdP.kind = kind;
	if (polar_vfs_file_before_hook)
		polar_vfs_file_before_hook(path, &vfdP, VFS_TRUNCATE);

	rc = vfs[kind]->vfs_truncate(vfs_path, len);

	VFS_RESUME_INTERRUPTS();

	return rc;
}

static DIR *
vfs_opendir(const char *dirname)
{
	DIR		   *dir = NULL;
	int			kind = -1;
	const char *vfs_path;
	int			save_errno;

	if (dirname == NULL)
		return NULL;

	if (!vfs_allocated_dir())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded max_vfs_dir_descs (%d) while trying to open directory \"%s\"",
						max_vfs_dir_descs, dirname)));

	VFS_HOLD_INTERRUPTS();

	vfs_path = polar_vfs_file_type_and_path(dirname, &kind);
	dir = vfs[kind]->vfs_opendir(vfs_path);
	save_errno = errno;
	if (dir != NULL)
	{
		vfs_dir_desc *desc = &vfs_dir_descs[num_vfs_dir_descs];

		desc->kind = kind;
		desc->dir = dir;
		num_vfs_dir_descs++;

		if (unlikely(polar_vfs_debug))
			elog(LOG, "vfs open dir %s, num open dir %d", dirname, num_vfs_dir_descs);

		VFS_RESUME_INTERRUPTS();
		errno = save_errno;
		return desc->dir;
	}

	VFS_RESUME_INTERRUPTS();

	errno = save_errno;
	return NULL;
}

static struct dirent *
vfs_readdir(DIR *dir)
{
	int			i;

	if (dir == NULL)
		return NULL;

	for (i = num_vfs_dir_descs; --i >= 0;)
	{
		vfs_dir_desc *desc = &vfs_dir_descs[i];

		if (desc->dir == dir)
			return vfs[desc->kind]->vfs_readdir(dir);
	}

	elog(WARNING, "vfs could not find vfs_desc");

	return NULL;
}

static int
vfs_closedir(DIR *dir)
{
	int			i;
	int			result = -1;
	int			save_errno;

	for (i = num_vfs_dir_descs; --i >= 0;)
	{
		vfs_dir_desc *desc = &vfs_dir_descs[i];

		if (desc->dir == dir)
		{
			result = vfs[desc->kind]->vfs_closedir(dir);
			save_errno = errno;
			num_vfs_dir_descs--;

			/* this is end of array */
			if (i == num_vfs_dir_descs)
			{
				desc->dir = NULL;
				desc->kind = POLAR_VFS_UNKNOWN_FILE;
			}
			else
			{
				/* Compact storage in the allocatedDescs array */
				*desc = vfs_dir_descs[num_vfs_dir_descs];
			}
			errno = save_errno;
			return result;
		}
	}

	elog(WARNING, "vfs does not find vfs_dir from vfs_dir_descs");

	return result;
}

static int
vfs_mkdir(const char *path, mode_t mode)
{
	int			rc = -1;
	int			kind = -1;
	const char *vfs_path;
	vfs_vfd		vfdP;

	if (path == NULL)
		return -1;

	vfs_path = polar_vfs_file_type_and_path(path, &kind);

	vfdP.kind = kind;
	if (polar_vfs_file_before_hook)
		polar_vfs_file_before_hook(path, &vfdP, VFS_MKDIR);

	rc = vfs[kind]->vfs_mkdir(vfs_path, mode);
	return rc;
}

static int
vfs_rmdir(const char *path)
{
	int			rc = -1;
	int			kind = -1;
	const char *vfs_path;
	vfs_vfd		vfdP;

	if (path == NULL)
		return -1;

	vfs_path = polar_vfs_file_type_and_path(path, &kind);

	vfdP.kind = kind;
	if (polar_vfs_file_before_hook)
		polar_vfs_file_before_hook(path, &vfdP, VFS_RMDIR);

	rc = vfs[kind]->vfs_rmdir(vfs_path);

	return rc;
}

/*
 * Determine which kind of device the file is on.
 * Besides, vfs_path is the available part of path which is pointing
 * to the target file.
 */
static inline const char *
polar_vfs_file_type_and_path(const char *path, int *kind)
{
	int			i;
	int			vfs_kind_len;
	const char *vfs_path = path;

	*kind = POLAR_VFS_UNKNOWN_FILE;
	if (path != NULL)
	{
		for (i = 0; i < POLAR_VFS_KIND_SIZE; i++)
		{
			vfs_kind_len = strlen(polar_vfs_kind[i]);
			if (strncmp(polar_vfs_kind[i], path, vfs_kind_len) == 0)
			{
				*kind = i;
				vfs_path = path + vfs_kind_len;
				break;
			}
		}
	}
	if (*kind == POLAR_VFS_UNKNOWN_FILE)
		*kind = vfs_file_type(path);
	return vfs_path;
}

/* Determine if the file is on a shared storage device */
static inline int
vfs_file_type(const char *path)
{
	static int	polar_disk_strsize = 0;
	int			strpathlen = 0;

	if (localfs_mode)
		return POLAR_VFS_LOCAL_BIO;

	if (path == NULL)
		return POLAR_VFS_LOCAL_BIO;

	if (polar_disk_strsize == 0 && polar_disk_name)
		polar_disk_strsize = strlen(polar_disk_name);

	strpathlen = strlen(path);
	if (strpathlen <= 1 || polar_disk_strsize <= 0)
		return POLAR_VFS_LOCAL_BIO;

	if (strpathlen < polar_disk_strsize + 1)
		return POLAR_VFS_LOCAL_BIO;

	if (path[0] != '/')
		return POLAR_VFS_LOCAL_BIO;

	if (strncmp(polar_disk_name, path + 1, polar_disk_strsize) == 0)
		return POLAR_VFS_PFS;

	return POLAR_VFS_LOCAL_BIO;
}

/* local file fd cache */
static inline int
vfs_allocate_vfd(void)
{
	Index		i;
	File		file;

	elog(DEBUG1, "vfs_allocate_vfd. cache size %zu", size_vfd_cache);

	Assert(size_vfd_cache > 0);

	if (vfs_vfd_cache[0].next_free == 0)
	{
		Size		new_cache_size = size_vfd_cache * 2;
		vfs_vfd    *new_vfd_cache;

		if (new_cache_size < 32)
			new_cache_size = 32;
		else if (new_cache_size >= POLAR_VFS_FD_MASK)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("too many fds to vfs_allocate_vfd")));

		new_vfd_cache = (vfs_vfd *) realloc(vfs_vfd_cache, sizeof(vfs_vfd) * new_cache_size);
		if (new_vfd_cache == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory vfs_allocate_vfd")));

		vfs_vfd_cache = new_vfd_cache;
		for (i = size_vfd_cache; i < new_cache_size; i++)
		{
			MemSet((char *) &(vfs_vfd_cache[i]), 0, sizeof(vfs_vfd));
			vfs_vfd_cache[i].next_free = i + 1;
			vfs_vfd_cache[i].fd = VFD_CLOSED;
		}
		vfs_vfd_cache[new_cache_size - 1].next_free = 0;
		vfs_vfd_cache[0].next_free = size_vfd_cache;
		size_vfd_cache = new_cache_size;
	}

	file = vfs_vfd_cache[0].next_free;
	vfs_vfd_cache[0].next_free = vfs_vfd_cache[file].next_free;

	return file;
}

static inline void
vfs_free_vfd(int file)
{
	vfs_vfd    *vfdP = &vfs_vfd_cache[file];

	elog(DEBUG1, "vfs_free_vfd: %d (%s)",
		 file, vfdP->file_name ? vfdP->file_name : "");

	if (vfdP->file_name != NULL)
	{
		free(vfdP->file_name);
		vfdP->file_name = NULL;
	}

	vfdP->kind = POLAR_VFS_UNKNOWN_FILE;
	vfdP->next_free = vfs_vfd_cache[0].next_free;
	vfs_vfd_cache[0].next_free = file;

}

static inline bool
vfs_allocated_dir(void)
{
	vfs_dir_desc *new_descs;
	int			new_max;

	if (num_vfs_dir_descs < max_vfs_dir_descs)
		return true;

	Assert(vfs_dir_descs);

	new_max = max_safe_fds / 2;
	if (new_max > max_vfs_dir_descs)
	{
		new_descs = (vfs_dir_desc *) realloc(vfs_dir_descs,
											 new_max * sizeof(vfs_dir_desc));
		if (new_descs == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory vfs_allocated_dir")));
		vfs_dir_descs = new_descs;
		max_vfs_dir_descs = new_max;
		return true;
	}

	return false;
}

static inline vfs_vfd *
vfs_find_file(int file)
{
	if (file >= size_vfd_cache)
		elog(ERROR, "vfs file fd %d out of cache", file);

	return &vfs_vfd_cache[file];
}

static const vfs_mgr *
vfs_get_mgr(const char *path)
{
	int			kind = POLAR_VFS_LOCAL_BIO;

	polar_vfs_file_type_and_path(path, &kind);
	return vfs[kind];
}

static int
vfs_chmod(const char *path, mode_t mode)
{
	int			rc = -1;
	int			kind = -1;
	const char *vfs_path;

	if (path == NULL)
		return -1;
	vfs_path = polar_vfs_file_type_and_path(path, &kind);
	if (vfs[kind]->vfs_chmod)
		rc = vfs[kind]->vfs_chmod(vfs_path, mode);
	return rc;
}

static void *
vfs_mmap(void *start, size_t length, int prot, int flags, int file, off_t offset)
{
	vfs_vfd    *vfdP = NULL;

	POLAR_VFS_FD_MASK_RMOVE(file);
	vfdP = vfs_find_file(file);

	return vfs[vfdP->kind]->vfs_mmap(start, length, prot, flags, vfdP->fd, offset);
}

static PolarVFSKind
vfs_type(int fd)
{
	vfs_vfd    *vfdP = NULL;

	POLAR_VFS_FD_MASK_RMOVE(fd);
	vfdP = vfs_find_file(fd);

	return vfs[vfdP->kind]->vfs_type(vfdP->fd);
}
