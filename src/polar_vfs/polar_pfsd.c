/*-------------------------------------------------------------------------
 *
 * polar_pfsd.c
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
 *	  src/polar_vfs/polar_pfsd.c
 *
 *-------------------------------------------------------------------------
 */
#include "polar_vfs/polar_pfsd.h"
#ifdef USE_PFSD
#include "pfsd_sdk.h"


static int	polar_transform_pfs_flag(int flag);

static int	polar_pfsd_remount(vfs_mount_arg_t *remount_arg);
static int	polar_pfsd_umount_force(char *ftype, const char *pbdname);
static int	polar_pfsd_mount(vfs_mount_arg_t *mount_arg);

static ssize_t polar_pfsd_read(int fd, void *buf, size_t len);
static ssize_t polar_pfsd_pread(int fd, void *buf, size_t len, off_t offset);
static ssize_t polar_pfsd_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
static ssize_t polar_pfsd_write(int fd, const void *buf, size_t len);
static ssize_t polar_pfsd_pwrite(int fd, const void *buf, size_t len, off_t offset);
static ssize_t polar_pfsd_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);

static int64 polar_pfsd_register_buffer(int memfd, size_t size, void *base);
static int	polar_pfsd_unregister_buffer(int64 buf_id);
#endif

int			max_pfsd_io_size = PFSD_DEFAULT_MAX_IOSIZE;

static inline PolarVFSKind
polar_pfsd_vfs_type(int fd)
{
	return POLAR_VFS_PFS;
}

/*
 * Pfsd file system interface.
 * It use original pfsd's file access interface.
 * We packaged (p)read/(p)write interface because of the upper
 * limit of content for one single pfsd_(p)read/pfsd_(p)write
 * should be under our control.
 */
const vfs_mgr polar_vfs_pfsd =
{
#ifdef USE_PFSD
	.vfs_env_init = NULL,
	.vfs_env_destroy = NULL,
	.vfs_mount = polar_pfsd_mount,
	.vfs_remount = polar_pfsd_remount,
	.vfs_umount = polar_pfsd_umount_force,
	.vfs_open = pfsd_open,
	.vfs_creat = pfsd_creat,
	.vfs_close = pfsd_close,
	.vfs_read = polar_pfsd_read,
	.vfs_write = polar_pfsd_write,
	.vfs_pread = polar_pfsd_pread,
	.vfs_preadv = polar_pfsd_preadv,
	.vfs_pwrite = polar_pfsd_pwrite,
	.vfs_pwritev = polar_pfsd_pwritev,
	.vfs_stat = pfsd_stat,
	.vfs_fstat = pfsd_fstat,
	.vfs_lstat = pfsd_stat,
	.vfs_lseek = pfsd_lseek,
	.vfs_access = pfsd_access,
	.vfs_fsync = pfsd_fsync,
	.vfs_unlink = pfsd_unlink,
	.vfs_rename = pfsd_rename,
	.vfs_posix_fallocate = pfsd_posix_fallocate,
	.vfs_fallocate = pfsd_fallocate,
	.vfs_ftruncate = pfsd_ftruncate,
	.vfs_truncate = pfsd_truncate,
	.vfs_opendir = pfsd_opendir,
	.vfs_readdir = pfsd_readdir,
	.vfs_closedir = pfsd_closedir,
	.vfs_mkdir = pfsd_mkdir,
	.vfs_rmdir = pfsd_rmdir,
	.vfs_mgr_func = NULL,
	.vfs_chmod = pfsd_chmod,
	.vfs_mmap = NULL,
	.vfs_type = polar_pfsd_vfs_type,
	.vfs_register_buffer = polar_pfsd_register_buffer,
	.vfs_unregister_buffer = polar_pfsd_unregister_buffer,
#else
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
	.vfs_mmap = NULL,
	.vfs_type = NULL,
#endif
};

#ifdef USE_PFSD
static int
polar_transform_pfs_flag(int flag)
{
	int			pfs_flag = 0x0;

	if (flag & POLAR_VFS_RD)
		pfs_flag |= PFS_RD;
	if (flag & POLAR_VFS_RDWR)
		pfs_flag |= PFS_RDWR;
	if (flag & POLAR_VFS_PAXOS_BYFORCE)
		pfs_flag |= PFS_PAXOS_BYFORCE;
	if (flag & POLAR_VFS_TOOL)
		pfs_flag |= PFS_TOOL;

	return pfs_flag;
}

/*
 * POLAR: the one zero-copy ("zc") registered region.
 *
 * Maps the pfsd-registered memfd [base, end) to its buf_id. A PFS read/write
 * whose buffer lies entirely inside it and meets sector alignment goes straight
 * out of the memfd via pfsd_pread_zc / pfsd_pwrite_zc; everything else copies.
 *
 * The whole main shared-memory segment is this single registered memfd
 * (polar_zc_main_segment_create), so one region covers every shmem buffer (WAL,
 * buffer pool, SLRU, copy buffers, checksum scratch). Registered once in the
 * postmaster before fork and mutated only there (prior generation unregistered
 * before the next is installed), so it needs no locking and one slot suffices.
 */
static struct ZcRegion
{
	const char *base;
	const char *end;
	uint64		buf_id;
	bool		active;
}			ZcRegion;

static int64
polar_pfsd_register_buffer(int memfd, size_t size, void *base)
{
	int64		buf_id = pfsd_register_shared_buffer(memfd, size);

	if (buf_id < 0)
		return -1;

	ZcRegion.base = (const char *) base;
	ZcRegion.end = (const char *) base + size;
	ZcRegion.buf_id = (uint64) buf_id;
	ZcRegion.active = true;
	return buf_id;
}

static int
polar_pfsd_unregister_buffer(int64 buf_id)
{
	if (ZcRegion.active && ZcRegion.buf_id == (uint64) buf_id)
	{
		ZcRegion.active = false;
		ZcRegion.base = NULL;
		ZcRegion.end = NULL;
	}

	return pfsd_unregister_shared_buffer(buf_id);
}

/*
 * If [buf, buf+len) lies inside the registered region, translate it to the
 * (buf_id, buf_off) the *_zc calls expect and return true. buf_off is the
 * offset within the region (i.e. relative to its mmap base), which is what pfsd
 * resolves against its own mapping of the same memfd.
 *
 * The zc path is O_DIRECT-style, so buf_off, len and the file offset must all
 * meet PG_IO_ALIGN_SIZE sector alignment. The whole segment is one region, so
 * an arbitrary write could land misaligned -- guard here and fall back to the
 * copying path (no alignment constraint) rather than let the server EINVAL.
 * Aligned consumers pass; sub-4K-blocksize IO always falls back. use_zc is
 * decided once per call, relying on max_pfsd_io_size (4 MiB) being a multiple
 * of the alignment to keep every chunk's offset aligned through the IO loop.
 */
static inline bool
polar_pfsd_zc_translate(const void *buf, size_t len, off_t off,
						uint64 *buf_id, off_t *buf_off)
{
	const struct ZcRegion *r = &ZcRegion;
	const char *p = (const char *) buf;
	off_t		boff;

	if (!r->active || p < r->base || p + len > r->end)
		return false;

	boff = (off_t) (p - r->base);

	/* sector-alignment guard: else fall back to the copying path */
	if (((boff | (off_t) len | off) & (PG_IO_ALIGN_SIZE - 1)) != 0)
		return false;

	*buf_id = r->buf_id;
	*buf_off = boff;
	return true;
}

static ssize_t
polar_pfsd_write(int fd, const void *buf, size_t len)
{
	ssize_t		res = -1;
	char	   *from;
	ssize_t		nleft;
	ssize_t		writesize;
	ssize_t		count = 0;

	nleft = len;
	from = (char *) buf;

	while (nleft > 0)
	{
		writesize = Min(nleft, max_pfsd_io_size);
		res = pfsd_write(fd, from, writesize);

		if (res <= 0)
		{
			if (count == 0)
				count = res;

			break;
		}

		count += res;
		nleft -= res;
		from += res;
	}

	return count;
}

static ssize_t
polar_pfsd_read(int fd, void *buf, size_t len)
{
	ssize_t		res = -1;
	ssize_t		iolen = 0;
	ssize_t		nleft = len;
	char	   *from = (char *) buf;
	ssize_t		count = 0;

	while (nleft > 0)
	{
		iolen = Min(nleft, max_pfsd_io_size);
		res = pfsd_read(fd, from, iolen);

		if (res <= 0)
		{
			if (count == 0)
				count = res;

			break;
		}

		count += res;
		from += res;
		nleft -= res;
	}

	return count;
}

static ssize_t
polar_pfsd_pread(int fd, void *buf, size_t len, off_t offset)
{
	ssize_t		res = -1;
	ssize_t		iolen = 0;
	off_t		off = offset;
	ssize_t		nleft = len;
	char	   *from = (char *) buf;
	ssize_t		count = 0;
	uint64		zc_buf_id = 0;
	off_t		zc_buf_off = 0;
	bool		use_zc = polar_pfsd_zc_translate(buf, len, offset, &zc_buf_id, &zc_buf_off);

	while (nleft > 0)
	{
		iolen = Min(nleft, max_pfsd_io_size);
		if (use_zc)
			res = pfsd_pread_zc(fd, zc_buf_id, zc_buf_off, iolen, off);
		else
			res = pfsd_pread(fd, from, iolen, off);

		if (res <= 0)
		{
			if (count == 0)
				count = res;

			break;
		}

		count += res;
		from += res;
		off += res;
		nleft -= res;
		zc_buf_off += res;
	}

	return count;
}

static ssize_t
polar_pfsd_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t		count = 0;
	ssize_t		part;
	int			i;

	for (i = 0; i < iovcnt; ++i)
	{
		part = polar_pfsd_pread(fd, iov[i].iov_base, iov[i].iov_len, offset);
		if (part < 0)
		{
			if (i == 0)
				return -1;
			else
				return count;
		}
		count += part;
		offset += part;
		if (part < iov[i].iov_len)
			return count;
	}
	return count;
}

static ssize_t
polar_pfsd_pwrite(int fd, const void *buf, size_t len, off_t offset)
{
	char	   *from;
	ssize_t		nleft;
	off_t		startoffset;
	ssize_t		writesize;
	ssize_t		res = -1;
	ssize_t		count = 0;
	uint64		zc_buf_id = 0;
	off_t		zc_buf_off = 0;
	bool		use_zc = polar_pfsd_zc_translate(buf, len, offset, &zc_buf_id, &zc_buf_off);

	nleft = len;
	from = (char *) buf;
	startoffset = offset;

	while (nleft > 0)
	{
		writesize = Min(nleft, max_pfsd_io_size);
		if (use_zc)
			res = pfsd_pwrite_zc(fd, zc_buf_id, zc_buf_off, writesize, startoffset);
		else
			res = pfsd_pwrite(fd, from, writesize, startoffset);

		if (res <= 0)
		{
			if (count == 0)
				count = res;

			break;
		}

		count += res;
		nleft -= res;
		from += res;
		startoffset += res;
		zc_buf_off += res;
	}

	return count;
}

static ssize_t
polar_pfsd_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t		count = 0;
	ssize_t		part;
	int			i;

	for (i = 0; i < iovcnt; ++i)
	{
		part = polar_pfsd_pwrite(fd, iov[i].iov_base, iov[i].iov_len, offset);
		if (part < 0)
		{
			if (i == 0)
				return -1;
			else
				return count;
		}
		count += part;
		offset += part;
		if (part < iov[i].iov_len)
			return count;
	}

	return count;
}

int
polar_pfsd_mount_growfs(const char *pbdname)
{
	return pfsd_mount_growfs(pbdname);
}

static int
polar_pfsd_remount(vfs_mount_arg_t *remount_arg)
{
	pfsd_mount_arg_t *pfsd_remount_arg = (pfsd_mount_arg_t *) remount_arg->mount_arg;

	return pfsd_remount(pfsd_remount_arg->cluster, pfsd_remount_arg->pbdname, pfsd_remount_arg->hostid, polar_transform_pfs_flag(pfsd_remount_arg->flags));
}

unsigned long
polar_pfsd_meta_version_get(void)
{
	return pfsd_meta_version_get();
}

const char *
polar_pfsd_build_version_get(void)
{
	return pfsd_build_version_get();
}

static int
polar_pfsd_umount_force(char *ftype, const char *pbdname)
{
	return pfsd_umount_force(pbdname);
}

static int
polar_pfsd_mount(vfs_mount_arg_t *mount_arg)
{
	pfsd_mount_arg_t *pfsd_mount_arg = (pfsd_mount_arg_t *) mount_arg->mount_arg;

	return pfsd_mount(pfsd_mount_arg->cluster, pfsd_mount_arg->pbdname, pfsd_mount_arg->hostid, polar_transform_pfs_flag(pfsd_mount_arg->flags));
}


#else
int
polar_pfsd_mount_growfs(const char *pbdname)
{
	return -1;
}

unsigned long
polar_pfsd_meta_version_get(void)
{
	return 0;
}

const char *
polar_pfsd_build_version_get(void)
{
	return "Not config pfsd";
}

#endif
