/*-------------------------------------------------------------------------
 *
 * polar_fd.h
 *	  PolarDB Virtual file descriptor definitions.
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
 *	  src/include/storage/polar_fd.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLARFD_H
#define POLARFD_H

#include <dirent.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include "utils/resowner.h"

#define	POLAR_VFS_SWITCH_LOCAL		0
#define	POLAR_VFS_SWITCH_PLUGIN		1

#define POLAR_VFS_UNKNOWN_FILE			-1
#define POLAR_VFS_PROTOCOL_MAX_LEN		64
#define POLAR_VFS_PROTOCOL_TAG			"://"
#define POLAR_VFS_PROTOCOL_LOCAL_BIO	"file://"
#define POLAR_VFS_PROTOCOL_PFS			"pfsd://"
#define POLAR_VFS_PROTOCOL_LOCAL_DIO	"file-dio://"

#define POLAR_BUFFER_ALIGN_LEN			(4096)
#define POLAR_BUFFER_EXTEND_SIZE(LEN)	(LEN + POLAR_BUFFER_ALIGN_LEN)
#define POLAR_BUFFER_ALIGN(LEN)			TYPEALIGN(POLAR_BUFFER_ALIGN_LEN, LEN)

/*
 * ploar VFS interface kind
 */
typedef enum PolarVFSKind
{
	POLAR_VFS_LOCAL_BIO = 0,
	POLAR_VFS_PFS,
	POLAR_VFS_LOCAL_DIO,
	/* NB: Define the size here for future maintenance */
	POLAR_VFS_KIND_SIZE
} PolarVFSKind;

typedef enum PolarNodeType
{
	POLAR_UNKNOWN = 0,
	POLAR_PRIMARY = 1,
	POLAR_REPLICA,
	POLAR_STANDBY,

	/*
	 * datamax mode with independent storage, datamax mode with shared storage
	 * is not supported
	 */
	POLAR_STANDALONE_DATAMAX
} PolarNodeType;

/* POLAR: string format of node type */
#define POLAR_UNKNOWN_STRING			"UNKNOWN"
#define POLAR_PRIMARY_STRING			"PRIMARY"
#define POLAR_REPLICA_STRING			"REPLICA"
#define POLAR_STANDBY_STRING			"STANDBY"
#define POLAR_STANDALONE_DATAMAX_STRING	"STANDALONE_DATAMAX"

/* polar vfs mount flag only occupies the higher 16 bits */
#define POLAR_VFS_UNKNOWN		0x00000
#define POLAR_VFS_RD			0x10000
#define POLAR_VFS_RDWR			0x20000
#define POLAR_VFS_PAXOS_BYFORCE	0x40000
#define POLAR_VFS_TOOL			0x80000

/*
 * XLogCtl->polar_node_state consists of two parts:
 * (1) the lower 16 bits means PolarNodeType of current node.
 * (2) the higher 16 bits means polar vfs state which represents
 * vfs mount/remount flag of current node.
 */
#define POLAR_STATE_NODE_TYPE_MASK	0x0000FFFF
#define POLAR_STATE_VFS_MASK		0xFFFF0000
#define POLAR_STATE_NODE_TYPE(state) ((state) & POLAR_STATE_NODE_TYPE_MASK)
#define POLAR_STATE_VFS(state) ((state) & POLAR_STATE_VFS_MASK)

typedef struct vfs_mount_arg_t
{
	char	   *ftype;
	void	   *mount_arg;
} vfs_mount_arg_t;

typedef struct pfsd_mount_arg_t
{
	char	   *cluster;
	char	   *pbdname;
	int			hostid;
	int			flags;
} pfsd_mount_arg_t;

#define PFSD_INIT_MOUNT_ARG(mount_arg, cluster_name, disk_name, host_id, flag) \
	do { \
		(mount_arg).cluster = (cluster_name); \
		(mount_arg).pbdname = (disk_name); \
		(mount_arg).hostid = (host_id); \
		(mount_arg).flags = (flag); \
	} while (0)

#define VFS_INIT_MOUNT_ARG(vfs_mount_arg, file_type, kind_mount_arg) \
	do { \
		(vfs_mount_arg).ftype = (file_type); \
		(vfs_mount_arg).mount_arg = (kind_mount_arg); \
	} while (0)

extern PolarNodeType polar_local_node_type;
extern uint32 polar_local_vfs_state;
extern int	polar_vfs_switch;
extern char *polar_datadir;
extern bool polar_enable_shared_storage_mode;
extern bool polar_vfs_is_dio_mode;

typedef int (*vfs_open_type) (const char *path, int flags, mode_t mode);

typedef struct vfs_mgr
{
	int			(*vfs_env_init) (void);
	int			(*vfs_env_destroy) (void);
	int			(*vfs_mount) (vfs_mount_arg_t *mount_arg);
	int			(*vfs_remount) (vfs_mount_arg_t *remount_arg);
	int			(*vfs_open) (const char *path, int flags, mode_t mode);
	int			(*vfs_creat) (const char *path, mode_t mode);
	int			(*vfs_close) (int fd);
	ssize_t		(*vfs_read) (int fd, void *buf, size_t len);
	ssize_t		(*vfs_write) (int fd, const void *buf, size_t len);
	ssize_t		(*vfs_pread) (int fd, void *buf, size_t len, off_t offset);
	ssize_t		(*vfs_preadv) (int fd, const struct iovec *iov, int iovcnt, off_t offset);
	ssize_t		(*vfs_pwrite) (int fd, const void *buf, size_t len, off_t offset);
	ssize_t		(*vfs_pwritev) (int fd, const struct iovec *iov, int iovcnt, off_t offset);
	int			(*vfs_stat) (const char *path, struct stat *buf);
	int			(*vfs_fstat) (int fd, struct stat *buf);
	int			(*vfs_lstat) (const char *path, struct stat *buf);
	off_t		(*vfs_lseek) (int fd, off_t offset, int whence);
	int			(*vfs_access) (const char *path, int mode);
	int			(*vfs_fsync) (int fd);
	int			(*vfs_unlink) (const char *path);
	int			(*vfs_rename) (const char *oldpath, const char *newpath);
	int			(*vfs_posix_fallocate) (int fd, off_t offset, off_t len);
	int			(*vfs_fallocate) (int fd, int mode, off_t offset, off_t len);
	int			(*vfs_ftruncate) (int fd, off_t len);
	int			(*vfs_truncate) (const char *path, off_t len);
	DIR		   *(*vfs_opendir) (const char *path);
	struct dirent *(*vfs_readdir) (DIR *dir);
	int			(*vfs_closedir) (DIR *dir);
	int			(*vfs_mkdir) (const char *path, mode_t mode);
	int			(*vfs_rmdir) (const char *path);
	const struct vfs_mgr *(*vfs_mgr_func) (const char *path);
	int			(*vfs_chmod) (const char *path, mode_t mode);
	void	   *(*vfs_mmap) (void *start, size_t length, int prot, int flags, int fd, off_t offsize);
	int			(*vfs_sync_file_range) (int fd, off_t offset, off_t nbytes, unsigned int flags);
	int			(*vfs_posix_fadvise) (int fd, off_t offset, off_t len, int advice);
	int			(*vfs_umount) (char *ftype, const char *pbdname);
	PolarVFSKind (*vfs_type) (int fd);
} vfs_mgr;

extern vfs_mgr polar_vfs[];

static inline PolarVFSKind
polar_bufferio_vfs_type(int fd)
{
	return POLAR_VFS_LOCAL_BIO;
}

extern ssize_t polar_read_line(int fd, void *buffer, size_t len);
extern int	polar_copy_file(char *fromfile, char *tofile, bool skiperr);
extern void polar_copydir(char *fromdir, char *todir, bool recurse, bool clean, bool skip_file_err);
extern struct dirent *polar_read_dir_ext(DIR *dir, const char *dirname, int elevel, int *err);

extern void polar_register_tls_cleanup(void);
extern void polar_validate_dir(char *path);
extern void polar_init_node_type(void);
extern PolarNodeType polar_get_node_type_by_file(void);

static inline int
polar_env_init(void)
{
	if (polar_vfs[polar_vfs_switch].vfs_env_init)
		return polar_vfs[polar_vfs_switch].vfs_env_init();

	return 0;
}

static inline int
polar_env_destroy(void)
{
	if (polar_vfs[polar_vfs_switch].vfs_env_destroy)
		return polar_vfs[polar_vfs_switch].vfs_env_destroy();

	return 0;
}

static inline int
polar_mount(vfs_mount_arg_t *mount_arg)
{
	int			ret = 0;

	if (polar_vfs[polar_vfs_switch].vfs_mount)
		ret = polar_vfs[polar_vfs_switch].vfs_mount(mount_arg);
	return ret;
}

static inline int
polar_remount(vfs_mount_arg_t *remount_arg)
{
	int			ret = 0;

	if (polar_vfs[polar_vfs_switch].vfs_remount)
		ret = polar_vfs[polar_vfs_switch].vfs_remount(remount_arg);
	return ret;
}

static inline int
polar_open(const char *path, int flags, mode_t mode)
{
	return polar_vfs[polar_vfs_switch].vfs_open(path, flags, mode);
}

static inline int
polar_creat(const char *path, mode_t mode)
{
	return polar_vfs[polar_vfs_switch].vfs_creat(path, mode);
}

static inline int
polar_close(int fd)
{
	return polar_vfs[polar_vfs_switch].vfs_close(fd);
}

static inline ssize_t
polar_read(int fd, void *buf, size_t len)
{
	return polar_vfs[polar_vfs_switch].vfs_read(fd, buf, len);
}

static inline ssize_t
polar_write(int fd, const void *buf, size_t len)
{
	return polar_vfs[polar_vfs_switch].vfs_write(fd, buf, len);
}

static inline ssize_t
polar_pread(int fd, void *buf, size_t len, off_t offset)
{
	return polar_vfs[polar_vfs_switch].vfs_pread(fd, buf, len, offset);
}

static inline ssize_t
polar_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	return polar_vfs[polar_vfs_switch].vfs_preadv(fd, iov, iovcnt, offset);
}

static inline ssize_t
polar_pwrite(int fd, const void *buf, size_t len, off_t offset)
{
	return polar_vfs[polar_vfs_switch].vfs_pwrite(fd, buf, len, offset);
}

static inline ssize_t
polar_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	return polar_vfs[polar_vfs_switch].vfs_pwritev(fd, iov, iovcnt, offset);
}

/* Replacing stat function, check all unlink when every merge code */
static inline int
polar_stat(const char *path, struct stat *buf)
{
	return polar_vfs[polar_vfs_switch].vfs_stat(path, buf);
}

static inline int
polar_fstat(int fd, struct stat *buf)
{
	return polar_vfs[polar_vfs_switch].vfs_fstat(fd, buf);
}

static inline int
polar_lstat(const char *path, struct stat *buf)
{
	return polar_vfs[polar_vfs_switch].vfs_lstat(path, buf);
}

static inline off_t
polar_lseek(int fd, off_t offset, int whence)
{
	return polar_vfs[polar_vfs_switch].vfs_lseek(fd, offset, whence);
}

static inline int
polar_access(const char *path, int mode)
{
	return polar_vfs[polar_vfs_switch].vfs_access(path, mode);
}

static inline int
polar_fsync(int fd)
{
	return polar_vfs[polar_vfs_switch].vfs_fsync(fd);
}

/* Replacing unlink function, check all unlink when every merge code */
static inline int
polar_unlink(const char *fname)
{
	return polar_vfs[polar_vfs_switch].vfs_unlink(fname);
}

static inline int
polar_rename(const char *oldfile, const char *newfile)
{
	return polar_vfs[polar_vfs_switch].vfs_rename(oldfile, newfile);
}

static inline int
polar_posix_fallocate(int fd, off_t offset, off_t len)
{
	return polar_vfs[polar_vfs_switch].vfs_posix_fallocate(fd, offset, len);
}

static inline int
polar_fallocate(int fd, int mode, off_t offset, off_t len)
{
	return polar_vfs[polar_vfs_switch].vfs_fallocate(fd, mode, offset, len);
}

static inline int
polar_ftruncate(int fd, off_t len)
{
	return polar_vfs[polar_vfs_switch].vfs_ftruncate(fd, len);
}

static inline int
polar_truncate(const char *path, off_t len)
{
	return polar_vfs[polar_vfs_switch].vfs_truncate(path, len);
}

static inline DIR *
polar_opendir(const char *path)
{
	return polar_vfs[polar_vfs_switch].vfs_opendir(path);
}

static inline struct dirent *
polar_readdir(DIR *dir)
{
	return polar_vfs[polar_vfs_switch].vfs_readdir(dir);
}

static inline int
polar_closedir(DIR *dir)
{
	return polar_vfs[polar_vfs_switch].vfs_closedir(dir);
}

static inline int
polar_mkdir(const char *path, mode_t mode)
{
	return polar_vfs[polar_vfs_switch].vfs_mkdir(path, mode);
}

/* Replacing rmdir function, check all unlink when every merge code */
static inline int
polar_rmdir(const char *path)
{
	return polar_vfs[polar_vfs_switch].vfs_rmdir(path);
}

static inline int
polar_chmod(const char *path, mode_t mode)
{
	int			rc = 0;

	if (polar_vfs[polar_vfs_switch].vfs_chmod)
		rc = polar_vfs[polar_vfs_switch].vfs_chmod(path, mode);
	return rc;
}

static inline const vfs_mgr *
polar_get_local_vfs_mgr(const char *path)
{
	return &polar_vfs[POLAR_VFS_SWITCH_LOCAL];
}

static inline bool
polar_file_exists(const char *path)
{
	struct stat st;

	return (polar_stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

/*
 * We provide this macro in order to remove protocol
 * from polar_datadir. The polar_datadir must conform to the format:
 * [protocol]://[path]
 *
 * Notice: The polar_datadir's protocol must be same as the polar_vfs_klind
 * inside polar_vfs.c. This macro should ONLY be used when you can't use polar_vfs
 * interface.
 */
static inline const char *
polar_path_remove_protocol(const char *path)
{
	const char *vfs_path = strstr(path, POLAR_VFS_PROTOCOL_TAG);

	if (vfs_path)
		return vfs_path + strlen(POLAR_VFS_PROTOCOL_TAG);
	else
		return path;
}

static inline void
polar_make_file_path_level3(char *path, const char *base, const char *file_path)
{
	if (polar_enable_shared_storage_mode)
		snprintf(path, MAXPGPATH, "%s/%s/%s", polar_datadir, base, file_path);
	else
		snprintf(path, MAXPGPATH, "%s/%s", base, file_path);

	return;
}

static inline void
polar_make_file_path_level2(char *path, const char *file_path)
{
	if (polar_enable_shared_storage_mode)
		snprintf(path, MAXPGPATH, "%s/%s", polar_datadir, file_path);
	else
		snprintf(path, MAXPGPATH, "%s", file_path);

	return;
}

static inline const vfs_mgr *
polar_vfs_mgr(const char *path)
{
	if (polar_vfs[polar_vfs_switch].vfs_mgr_func)
		return (polar_vfs[polar_vfs_switch].vfs_mgr_func) (path);
	return NULL;
}

static inline int
polar_sync_file_range(int fd, off_t offset, off_t nbytes, unsigned int flags)
{
	int			rc = 0;

	if (polar_vfs[polar_vfs_switch].vfs_sync_file_range)
		rc = polar_vfs[polar_vfs_switch].vfs_sync_file_range(fd, offset, nbytes, flags);
	return rc;
}

static inline void *
polar_mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
	return polar_vfs[polar_vfs_switch].vfs_mmap(start, length, prot, flags, fd, offset);
}

static inline int
polar_posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
	int			rc = 0;

	if (polar_vfs[polar_vfs_switch].vfs_posix_fadvise)
		rc = polar_vfs[polar_vfs_switch].vfs_posix_fadvise(fd, offset, len, advice);
	return rc;
}

static inline int
polar_umount(char *ftype, const char *pbdname)
{
	int			rc = 0;

	if (polar_vfs[polar_vfs_switch].vfs_umount)
		rc = polar_vfs[polar_vfs_switch].vfs_umount(ftype, pbdname);

	return rc;
}

static inline PolarVFSKind
polar_vfs_type(int fd)
{
	return polar_vfs[polar_vfs_switch].vfs_type(fd);
}

#endif
