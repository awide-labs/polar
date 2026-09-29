/*-------------------------------------------------------------------------
 *
 * datamax_get_wal_from_backup.c - datamax get wal file from backup set
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
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
 *		  src/bin/polar_tools/datamax_get_wal_from_backup.c
 *-------------------------------------------------------------------------
 */

#define FRONTEND 1
#include "postgres.h"

#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "access/xlog_internal.h"
#include "access/transam.h"
#include "catalog/pg_control.h"
#include "common/file_perm.h"
#include "common/string.h"
#include "getopt_long.h"
#include "polar_datamax/polar_datamax_internal.h"
#include "polar_tools.h"
#include "polar_vfs/polar_vfs_fe.h"

static int	wal_segment_size = 16 * 1024 * 1024;
static bool datamax_pfs_mode = false;
static bool backup_pfs_mode = false;

typedef struct polar_verify_xlog_private
{
	TimeLineID	timeline;
	char	   *path;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	bool		endptr_reached;
} polar_verify_xlog_private;

static struct option long_options[] =
{
	{"pgdata", required_argument, NULL, 'D'},
	{"backup_waldir", required_argument, NULL, 'b'},
	{"datamax_mode", required_argument, NULL, 'M'},
	{"backup_mode", required_argument, NULL, 'm'},
	{"polar_disk_name_datamax", required_argument, NULL, 'N'},
	{"polar_disk_name_backup", required_argument, NULL, 'n'},
	{"polar_storage_cluster_name", required_argument, NULL, 's'},
	{"polar_hostid", required_argument, NULL, 'i'},
	{"help", no_argument, NULL, '?'},
	{NULL, 0, NULL, 0}
};

static void
usage(void)
{
	printf("DataMax get wal from backup usage:\n");
	printf("-D, --pgdata=datamax_dir			Specify datamax data directory\n");
	printf("-b, --backup_waldir=backup_dir  	Specify wal backup directory\n");
	printf("-M, --datamax_mode=l|p 				Specify datamax storage mode, l for local_mode, p for pfs_mode, default l\n");
	printf("-m, --backup_mode=l|p				Specify backup storage mode, l for local_mode, p for pfs_mode, default l\n");
	printf("-N, --polar_disk_name_datamax=disk	Specify datamax polar_disk_name for pfs mode\n");
	printf("-n, --polar_disk_name_backup=disk	Specify backup polar_disk_name for pfs mode\n");
	printf("-s, --polar_storage_cluster_name=cluster_name  Specify polar_storage_cluster_name for pfs mode\n");
	printf("-i, --polar_hostid=host_id, 		Specify polar_host_id for pfs mode\n");
	printf("-?, --help show this help, then exit\n");
}

static int
polar_validate_datamax_dir(char *datamax_datadir)
{
	char		path[MAXPGPATH];
	int			i = 0;
	struct stat stat_buf;
	const char *dirs[] =
	{
		POLAR_DATAMAX_DIR,
		POLAR_DATAMAX_WAL_DIR
	};

	for (i = 0; i < lengthof(dirs); ++i)
	{
		snprintf((path), MAXPGPATH, "%s/%s", datamax_datadir, dirs[i]);
		/* check and create dir */
		if (polar_stat(path, &stat_buf) == 0)
		{
			if (!S_ISDIR(stat_buf.st_mode))
			{
				fprintf(stderr, _("path \"%s\" is not a directory: %s\n"), path, strerror(errno));
				return -1;
			}
		}
		else
		{
			if (polar_mkdir(path, pg_dir_create_mode) < 0)
			{
				fprintf(stderr, _("could not create directory \"%s\": %s\n"), path, strerror(errno));
				return -1;
			}
		}
	}
	return 0;
}

static int
polar_get_smallest_and_greatest_walfile(char *waldir_path, char *smallest_walfile, char *greatest_walfile)
{
	DIR		   *xldir = NULL;
	struct dirent *xlde = NULL;
	int			found_smallest = 0,
				found_greatest = 0;

	if (waldir_path == NULL)
	{
		fprintf(stderr, "Error: parameter wal directory path is NULL\n");
		return -1;
	}

	if (smallest_walfile == NULL)
	{
		fprintf(stderr, "Error: parameter smallest_walfile is null pointer\n");
		return -1;
	}

	if (greatest_walfile == NULL)
	{
		fprintf(stderr, "Error: parameter greatest_walfile is null pointer\n");
		return -1;
	}

	xldir = polar_opendir(waldir_path);
	while ((xlde = polar_readdir(xldir)) != NULL)
	{
		if (!IsXLogFileName(xlde->d_name))
			continue;

		/* first found a valid wal file or found smaller wal file, update */
		if (!found_smallest || strcmp(xlde->d_name + 8, smallest_walfile + 8) < 0)
		{
			found_smallest = 1;
			strlcpy(smallest_walfile, xlde->d_name, XLOG_FNAME_LEN + 1);
		}

		/* first found a valid wal file or found greater wal file, update */
		if (!found_greatest || strcmp(xlde->d_name + 8, greatest_walfile + 8) > 0)
		{
			found_greatest = 1;
			strlcpy(greatest_walfile, xlde->d_name, XLOG_FNAME_LEN + 1);
		}
	}
	polar_closedir(xldir);
	smallest_walfile[XLOG_FNAME_LEN] = '\0';
	greatest_walfile[XLOG_FNAME_LEN] = '\0';

	return (found_smallest & found_greatest);
}

static int
polar_copy_wal_file(char *src_dir, char *dest_dir, char *filename)
{
#define POLAR_XLOG_SIZE_1MB  1024 * 1024

	char		src_path[MAXPGPATH];
	char		dest_path[MAXPGPATH];
	int			src_fd = -1,
				dest_fd = -1;
	char	   *buffer = NULL;
	int			nbytes;
	int			ret = -1;

	snprintf(src_path, MAXPGPATH, "%s/%s", src_dir, filename);
	snprintf(dest_path, MAXPGPATH, "%s/%s", dest_dir, filename);

	src_fd = polar_open(src_path, O_RDONLY | PG_BINARY, pg_file_create_mode);
	if (src_fd < 0)
	{
		fprintf(stderr, _("could not open file \"%s\": %s\n"), src_path, strerror(errno));
		goto end;
	}

	/* create wal file in dest_dir */
	dest_fd = polar_open(dest_path, O_RDWR | O_CREAT | PG_BINARY, pg_file_create_mode);
	if (dest_fd < 0)
	{
		/* no cover line */
		fprintf(stderr, _("could not open file \"%s\": %s\n"), dest_path, strerror(errno));
		goto end;
	}

	buffer = palloc0(sizeof(char) * POLAR_XLOG_SIZE_1MB);
	if (buffer == NULL)
	{
		/* no cover line */
		fprintf(stderr, "allocate memory for buffer error\n");
		goto end;
	}

	/* copy wal data from src_file to dest_file */
	for (nbytes = 0; nbytes < wal_segment_size; nbytes += POLAR_XLOG_SIZE_1MB)
	{
		if (polar_read(src_fd, buffer, POLAR_XLOG_SIZE_1MB) != POLAR_XLOG_SIZE_1MB)
		{
			/* no cover line */
			fprintf(stderr, _("could not read file \"%s\": %s\n"), src_path, strerror(errno));
			goto end;
		}
		if (polar_write(dest_fd, buffer, POLAR_XLOG_SIZE_1MB) != POLAR_XLOG_SIZE_1MB)
		{
			/* no cover line */
			fprintf(stderr, _("could not write to file \"%s\": %s\n"), dest_path, strerror(errno));
			goto end;
		}
	}

	if (polar_fsync(dest_fd) != 0)
	{
		/* no cover line */
		fprintf(stderr, _("could not fsync file \"%s\": %s\n"), dest_path, strerror(errno));
		goto end;
	}
	ret = 0;

end:
	if (buffer)
		pfree(buffer);
	if (src_fd >= 0)
		polar_close(src_fd);
	if (dest_fd >= 0)
		polar_close(dest_fd);
	return ret;
}

/*
 * read count bytes from a segment file, store the data in the passed buffer
 */
static void
polar_xlog_read(char *waldir, TimeLineID tli, XLogRecPtr start_ptr, char *buf, Size count)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes;
	char		path[MAXPGPATH];
	char		fname[MAXFNAMELEN];

	static int	read_file = -1;
	static XLogSegNo read_segNo = 0;
	static uint32 read_off = 0;

	p = buf;
	recptr = start_ptr;
	nbytes = count;

	while (nbytes > 0)
	{
		uint32		start_off;
		int			seg_bytes;
		int			read_bytes;

		start_off = XLogSegmentOffset(recptr, wal_segment_size);

		if (read_file < 0 || !XLByteInSeg(recptr, read_segNo, wal_segment_size))
		{
			/* switch to a new segment */
			if (read_file >= 0)
				polar_close(read_file);

			XLByteToSeg(recptr, read_segNo, wal_segment_size);

			/* get the wal segment file path */
			XLogFileName(fname, tli, read_segNo, wal_segment_size);
			snprintf(path, MAXPGPATH, "%s/%s", waldir, fname);

			if ((read_file = polar_open(path, O_RDONLY | PG_BINARY, 0)) == -1)
			{
				fprintf(stderr, _("could not open file \"%s\": %s\n"), path, strerror(errno));
				goto err;
			}
			read_off = 0;
		}

		/* need to seek in the file */
		if (read_off != start_off)
		{
			if (polar_lseek(read_file, (off_t) start_off, SEEK_SET) < 0)
			{
				XLogFileName(fname, tli, read_segNo, wal_segment_size);
				snprintf(path, MAXPGPATH, "%s/%s", waldir, fname);
				/* report error */
				fprintf(stderr, _("could not seek in file \"%s\": %s\n"), path, strerror(errno));
				goto err;
			}
			read_off = start_off;
		}

		/* how many bytes are within this segment */
		if (nbytes > (wal_segment_size - start_off))
			seg_bytes = wal_segment_size - start_off;
		else
			seg_bytes = nbytes;

		read_bytes = polar_read(read_file, p, seg_bytes);
		if (read_bytes <= 0)
		{
			XLogFileName(fname, tli, read_segNo, wal_segment_size);
			snprintf(path, MAXPGPATH, "%s/%s", waldir, fname);
			/* report error */
			fprintf(stderr, _("could not read from file \"%s\": %s\n"), path, strerror(errno));
			goto err;
		}

		/* update state for read */
		recptr += read_bytes;
		read_off += read_bytes;
		nbytes -= read_bytes;
		p += read_bytes;
	}
	return;

err:
	if (read_file >= 0)
	{
		polar_close(read_file);
		read_file = -1;
	}
	return;
}

/*
 * XLogReader read_page callback (no cur_file_tli out-param; set
 * state->seg.ws_tli directly instead).
 */
static int
polar_xlog_read_page(XLogReaderState *state, XLogRecPtr target_page_ptr, int req_len,
					 XLogRecPtr target_ptr, char *read_buff)
{
	polar_verify_xlog_private *private = state->private_data;
	int			count = XLOG_BLCKSZ;

	if (target_page_ptr + XLOG_BLCKSZ <= private->endptr)
		count = XLOG_BLCKSZ;
	else if (target_page_ptr + req_len <= private->endptr)
		count = private->endptr - target_page_ptr;
	else
	{
		private->endptr_reached = true;
		return -1;
	}

	polar_xlog_read(private->path, private->timeline, target_page_ptr, read_buff, count);
	state->seg.ws_tli = private->timeline;
	return count;
}

/*
 * XLogReaderAllocate() requires non-NULL segment_open / segment_close
 * callbacks in XLogReaderRoutine, but neither is reachable on our code path:
 *
 *   - The framework only invokes them via WALRead() (the helper page_read
 *     implementations opt into when they want the framework to own segment
 *     file lifetime) and via XLogReaderFree() cleanup when state->seg.ws_file
 *     is left open (>= 0).
 *
 *   - Our page_read callback (polar_xlog_read_page) calls polar_xlog_read
 *     instead of WALRead. polar_xlog_read manages its own segment fd in a
 *     function-local static (read_file / read_segNo / read_off) and uses
 *     polar_open / polar_close / polar_lseek directly - it never assigns
 *     state->seg.ws_file. WALOpenSegmentInit() leaves ws_file at -1, so
 *     XLogReaderFree() also skips its segment_close call.
 *
 * The stubs exist only to satisfy the XLogReaderAllocate() non-NULL contract;
 * they become real polar_open / polar_close wrappers only if polar_xlog_read
 * is ever rewritten to delegate segment lifetime to WALRead().
 */
static void
polar_xlog_segment_open(XLogReaderState *state, XLogSegNo nextSegNo, TimeLineID *tli_p)
{
	Assert(false);
}

static void
polar_xlog_segment_close(XLogReaderState *state)
{
	Assert(false);
}

/*
 * verify the validation of walfile in backup_dir
 */
static int
polar_verify_walfile(char *backup_dir, char *backup_smallest, char *backup_greatest)
{
	XLogReaderState *xlogreader_state = NULL;
	XLogReaderRoutine routine;
	polar_verify_xlog_private private;
	XLogRecord *record;
	XLogRecPtr	lsn_verify_from;
	XLogSegNo	segno;
	char	   *errormsg = NULL;
	int			ret = -1;

	/* get the start_lsn and end_lsn we need to verify */
	XLogFromFileName(backup_smallest, &private.timeline, &segno, wal_segment_size);
	XLogSegNoOffsetToRecPtr(segno, 0, wal_segment_size, private.startptr);
	XLogFromFileName(backup_greatest, &private.timeline, &segno, wal_segment_size);
	XLogSegNoOffsetToRecPtr(segno + 1, 0, wal_segment_size, private.endptr);
	private.path = backup_dir;
	private.endptr_reached = false;
	if (private.endptr == InvalidXLogRecPtr)
	{
		fprintf(stderr, "failed to verify wal: invalid endptr\n");
		goto end;
	}

	routine.page_read = polar_xlog_read_page;
	routine.segment_open = polar_xlog_segment_open;
	routine.segment_close = polar_xlog_segment_close;

	xlogreader_state = XLogReaderAllocate(wal_segment_size, backup_dir, &routine, &private);
	if (!xlogreader_state)
	{
		fprintf(stderr, "allocate xlogreader fail, out of memory\n");
		goto end;
	}

	/* first find a valid recptr to verify from */
	XLogBeginRead(xlogreader_state, private.startptr);
	lsn_verify_from = XLogFindNextRecord(xlogreader_state, private.startptr);
	if (lsn_verify_from == InvalidXLogRecPtr)
	{
		fprintf(stderr, _("could not find a valid record after %X/%X\n"), (uint32) (private.startptr >> 32), (uint32) private.startptr);
		goto end;
	}

	/* verify the xlog record */
	XLogBeginRead(xlogreader_state, lsn_verify_from);
	for (;;)
	{
		record = XLogReadRecord(xlogreader_state, &errormsg);
		/* record is null or reach the end of valid xlog record */
		if (!record || private.endptr_reached)
			break;
	}
	if (errormsg == NULL && private.endptr_reached)
		ret = 0;
	else if (errormsg)
		fprintf(stderr, _("error in wal record at %X/%X:%s\n"), (uint32) (xlogreader_state->ReadRecPtr >> 32), (uint32) xlogreader_state->ReadRecPtr, errormsg);

end:
	if (xlogreader_state)
		XLogReaderFree(xlogreader_state);
	return ret;
}

static pg_crc32c
polar_calc_meta_crc(unsigned char *data, size_t len)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, data, len);
	FIN_CRC32C(crc);

	return crc;
}

static int
polar_update_datamax_meta_file(char *datamax_datadir, char *backup_smallest, char *backup_greatest)
{
	char		datamax_meta_path[MAXPGPATH];
	struct stat stat_buf;
	polar_datamax_meta_data_t meta;
	pg_crc32c	crc;
	int			datamax_meta_fd = -1;
	TimeLineID	tmp_tli;
	XLogRecPtr	tmp_lsn;
	XLogSegNo	tmp_segno;
	bool		create_meta = false,
				ret = false;

	snprintf(datamax_meta_path, MAXPGPATH, "%s/%s/%s", datamax_datadir, POLAR_DATAMAX_DIR, POLAR_DATAMAX_META_FILE);
	/* read meta data */
	if (polar_stat(datamax_meta_path, &stat_buf) != 0)
	{
		/* create the meta file if it doesn't exist */
		datamax_meta_fd = polar_open(datamax_meta_path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, pg_file_create_mode);
		if (datamax_meta_fd < 0)
		{
			/* no cover line */
			fprintf(stderr, _("could not create file \"%s\": %s\n"), datamax_meta_path, strerror(errno));
			goto end;
		}
		create_meta = true;
		/* init meta data */
		MemSet(&meta, 0, sizeof(polar_datamax_meta_data_t));
		meta.magic = POLAR_DATAMAX_MAGIC;
		meta.version = POLAR_DATAMAX_VERSION;
	}
	else
	{
		datamax_meta_fd = polar_open(datamax_meta_path, O_RDWR | PG_BINARY, pg_file_create_mode);
		if (datamax_meta_fd < 0)
		{
			fprintf(stderr, _("could not open file \"%s\": %s\n"), datamax_meta_path, strerror(errno));
			goto end;
		}
		if (polar_read(datamax_meta_fd, &meta, sizeof(polar_datamax_meta_data_t)) != sizeof(polar_datamax_meta_data_t))
		{
			/* no cover line */
			fprintf(stderr, _("could not read file \"%s\": %s\n"), datamax_meta_path, strerror(errno));
			goto end;
		}

		/* verify the mata if it exists already	*/
		if (meta.magic != POLAR_DATAMAX_MAGIC)
		{
			fprintf(stderr, _("The magic number of meta file is incorrect, got %d, expect %d"), meta.magic, POLAR_DATAMAX_MAGIC);
			goto end;
		}

		if (meta.version != POLAR_DATAMAX_VERSION)
		{
			fprintf(stderr, _("The version is incorrect, got %d, expect %d"), meta.version, POLAR_DATAMAX_VERSION);
			goto end;
		}

		/* check crc value */
		crc = meta.crc;
		meta.crc = 0;
		meta.crc = polar_calc_meta_crc((unsigned char *) &meta, sizeof(polar_datamax_meta_data_t));
		if (crc != meta.crc)
		{
			fprintf(stderr, "The crc is incorrect, got %u, expect %u", crc, meta.crc);
			goto end;
		}
	}

	/* update min received info */
	if (backup_smallest != NULL)
	{
		XLogFromFileName(backup_smallest, &tmp_tli, &tmp_segno, wal_segment_size);
		XLogSegNoOffsetToRecPtr(tmp_segno, 0, wal_segment_size, tmp_lsn);
		if (meta.min_received_lsn == InvalidXLogRecPtr || tmp_lsn < meta.min_received_lsn)
		{
			meta.min_received_lsn = tmp_lsn;
			meta.min_timeline_id = tmp_tli;
			fprintf(stdout, _("update meta min timeline id:%d, min received lsn:%lX\n"), meta.min_timeline_id, meta.min_received_lsn);
		}
	}
	/* update last received info */
	if (backup_greatest != NULL)
	{
		XLogFromFileName(backup_greatest, &tmp_tli, &tmp_segno, wal_segment_size);
		XLogSegNoOffsetToRecPtr(tmp_segno + 1, 0, wal_segment_size, tmp_lsn);
		if (tmp_lsn > meta.last_received_lsn)
		{
			meta.last_received_lsn = tmp_lsn;
			meta.last_timeline_id = tmp_tli;
			meta.last_valid_received_lsn = meta.last_received_lsn;
			fprintf(stdout, _("update meta last timeline id:%d, last received lsn:%lX, last valid received lsn:%lX\n"),
					meta.last_timeline_id, meta.last_received_lsn, meta.last_valid_received_lsn);
		}
	}
	/* caculate new meta crc value */
	meta.crc = 0;
	meta.crc = polar_calc_meta_crc((unsigned char *) &meta, sizeof(polar_datamax_meta_data_t));

	/* update meta file */
	if (polar_lseek(datamax_meta_fd, 0, SEEK_SET) < 0)
	{
		/* no cover line */
		fprintf(stderr, _("could not seek in file \"%s\": %s\n"), datamax_meta_path, strerror(errno));
		goto end;
	}
	if (polar_write(datamax_meta_fd, &meta, sizeof(polar_datamax_meta_data_t)) != sizeof(polar_datamax_meta_data_t))
	{
		/* no cover line */
		fprintf(stderr, _("could not write to file \"%s\": %s\n"), datamax_meta_path, strerror(errno));
		goto end;
	}
	if (polar_fsync(datamax_meta_fd) != 0)
	{
		/* no cover line */
		fprintf(stderr, _("could not fsync file \"%s\": %s\n"), datamax_meta_path, strerror(errno));
		goto end;
	}
	ret = true;

end:
	if (datamax_meta_fd >= 0)
		polar_close(datamax_meta_fd);
	if (create_meta && !ret)
		polar_unlink(datamax_meta_path);
	return ret ? 0 : -1;
}

/*
 * copy wal file from backup_dir to datamax_datadir
 */
static int
polar_datamax_copy_wal_from_backup(char *backup_dir, char *datamax_datadir)
{
	char	   *backup_smallest = NULL;
	char	   *backup_greatest = NULL;
	char		datamax_waldir[MAXPGPATH];
	DIR		   *xldir = NULL;
	struct dirent *xlde = NULL;
	int			getwal_ret = 0,
				ret = -1;

	/* get current smallest and biggest wal segment file in backup waldir */
	backup_smallest = malloc(XLOG_FNAME_LEN + 1);
	if (!backup_smallest)
	{
		fprintf(stderr, "allocate memory for backup_smallest error\n");
		goto end;
	}
	backup_greatest = malloc(XLOG_FNAME_LEN + 1);
	if (!backup_greatest)
	{
		fprintf(stderr, "allocate memory for backup_greatest error\n");
		goto end;
	}

	getwal_ret = polar_get_smallest_and_greatest_walfile(backup_dir, backup_smallest, backup_greatest);
	if (getwal_ret < 0)
	{
		fprintf(stderr, "failed to get smallest and greatest walfile in backup waldir, invalid parameter\n");
		goto end;
	}
	else if (getwal_ret == 0)
	{
		fprintf(stderr, "there is no wal file in backup set, no need to copy\n");
		goto end;
	}
	fprintf(stdout, _("backup_set smallest_walfile:%s, greatest_walfile:%s\n"), backup_smallest, backup_greatest);

	/* verify the wal file in backup_waldir */
	fprintf(stdout, "check the validation of wal in backup_set\n");
	if (polar_verify_walfile(backup_dir, backup_smallest, backup_greatest) < 0)
	{
		fprintf(stderr, "invalid walfile in backup_waldir \"%s\"\n", backup_dir);
		goto end;
	}

	/*
	 * after checking the validation of wal in backup_dir, copy them to
	 * datamax_waldir
	 */
	fprintf(stdout, "wal in backup_set is valid, copy them to datamax datadir\n");
	snprintf(datamax_waldir, MAXPGPATH, "%s/%s", datamax_datadir, POLAR_DATAMAX_WAL_DIR);
	xldir = polar_opendir(backup_dir);
	while ((xlde = polar_readdir(xldir)) != NULL)
	{
		if (!IsXLogFileName(xlde->d_name))
			continue;
		fprintf(stdout, _("copy file %s from %s to %s\n"), xlde->d_name, backup_dir, datamax_waldir);
		if (polar_copy_wal_file(backup_dir, datamax_waldir, xlde->d_name) != 0)
		{
			fprintf(stderr, _("copy wal file %s from %s to %s error\n"), xlde->d_name, backup_dir, datamax_waldir);
			goto end;
		}
	}

	/* update meta file of datamax */
	fprintf(stdout, "update datamax meta file\n");
	if (polar_update_datamax_meta_file(datamax_datadir, backup_smallest, backup_greatest) < 0)
	{
		fprintf(stderr, "failed to update datamax meta file\n");
		goto end;
	}
	ret = 0;

end:
	if (backup_smallest)
		free(backup_smallest);
	if (backup_greatest)
		free(backup_greatest);
	if (xldir)
		polar_closedir(xldir);
	return ret;
}

static int
polar_get_wal_segment_size(char *control_file)
{
	ControlFileData *control_data = NULL;
	int			fd = -1,
				wal_size = 0;
	pg_crc32c	crc_value;

	control_data = palloc0(sizeof(ControlFileData));
	if (!control_data)
	{
		fprintf(stderr, "allocate memory for control_data error\n");
		goto end;
	}

	fd = polar_open(control_file, O_RDONLY | PG_BINARY, pg_file_create_mode);
	if (fd < 0)
	{
		fprintf(stderr, _("could not open file \"%s\": %s\n"), control_file, strerror(errno));
		goto end;
	}
	if (polar_read(fd, control_data, sizeof(ControlFileData)) != sizeof(ControlFileData))
	{
		fprintf(stderr, _("could not read datamax control file \"%s\": %s\n"), control_file, strerror(errno));
		goto end;
	}

	/* check the CRC */
	crc_value = polar_calc_meta_crc((unsigned char *) control_data, offsetof(ControlFileData, crc));

	if (!EQ_CRC32C(crc_value, control_data->crc))
	{
		fprintf(stderr, "Failed to check crc, expected %X but got %X\n", control_data->crc, crc_value);
		goto end;
	}

	/* Make sure the control file is valid byte order */
	if (control_data->pg_control_version % 65536 == 0 &&
		control_data->pg_control_version / 65536 != 0)
	{
		/* no cover line */
		fprintf(stderr, "WARNING: possible byte ordering mismatch\n"
				"The byte ordering used to store the pg_control file might not match the one\n"
				"used by this program.  In that case the results below would be incorrect, and\n"
				"the PostgreSQL installation would be incompatible with this data directory.\n");
		goto end;
	}
	/* return wal segment size */
	wal_size = control_data->xlog_seg_size;

end:
	if (control_data)
		pfree(control_data);
	if (fd >= 0)
		polar_close(fd);
	return wal_size;
}

int
datamax_get_wal_main(int argc, char **argv)
{
	int			option,
				optindex = 0;
	char	   *datamax_datadir = NULL;
	char	   *backup_waldir = NULL;
	char	   *polar_storage_cluster_name = NULL;
	char	   *polar_disk_name_datamax = NULL;
	char	   *polar_disk_name_backup = NULL;
	int			polar_hostid = 0;
	char		datamax_control_filepath[MAXPGPATH];
	struct stat stat_buf;
	bool		init_datamax_fs = false,
				init_backup_fs = false;
	bool		succeed = false;

	if (argc <= 4)
	{
		usage();
		return -1;
	}

	while ((option = getopt_long(argc, argv, "D:b:M:m:N:n:s:i:?", long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'D':
				datamax_datadir = pg_strdup(optarg);
				break;
			case 'b':
				backup_waldir = pg_strdup(optarg);
				break;
			case 'M':
				if (strcmp(optarg, "l") == 0 || strcmp(optarg, "local") == 0)
					datamax_pfs_mode = false;
				else if (strcmp(optarg, "p") == 0 || strcmp(optarg, "pfs") == 0)
					datamax_pfs_mode = true;
				else
				{
					fprintf(stderr,
							_("invalid storage mode \"%s\", must be \"local\" or \"pfs\"\n"), optarg);
					goto end;
				}
				break;
			case 'm':
				if (strcmp(optarg, "l") == 0 || strcmp(optarg, "local") == 0)
					backup_pfs_mode = false;
				else if (strcmp(optarg, "p") == 0 || strcmp(optarg, "pfs") == 0)
					backup_pfs_mode = true;
				else
				{
					fprintf(stderr,
							_("invalid storage mode \"%s\", must be \"local\" or \"pfs\"\n"), optarg);
					goto end;
				}
				break;
			case 'N':
				polar_disk_name_datamax = pg_strdup(optarg);
				break;
			case 'n':
				polar_disk_name_backup = pg_strdup(optarg);
				break;
			case 's':
				polar_storage_cluster_name = pg_strdup(optarg);
				break;
			case 'i':
				polar_hostid = atoi(optarg);
				break;
			case '?':
				usage();
				exit(0);
				break;
			default:
				usage();
				goto end;
		}
	}

	/* check parameter for pfs mode */
	if (datamax_pfs_mode || backup_pfs_mode)
	{
		/*
		 * default value of polar_storage_cluster_name is polarstore if not
		 * specified, so it can be NULL
		 */
		if (polar_hostid <= 0)
		{
			fprintf(stderr, "polar_hostid need to be specified in pfs mode\n");
			goto end;
		}
		else if (polar_disk_name_datamax == NULL && polar_disk_name_backup == NULL)
		{
			fprintf(stderr, "polar_disk_name (datamax or backup) need to be specified in pfs mode\n");
			goto end;
		}
	}

	/*
	 * The polar_open/read/write/close inlines dispatch via a single
	 * polar_vfs_switch global, not a per-call pfs_mode arg, so we cannot have
	 * the datamax dir on PFS and the backup dir on local-fs (or vice versa)
	 * within the same process without state swapping.  Both must share the
	 * same storage mode here; reject the mixed case explicitly rather than
	 * silently misrouting one side.
	 */
	if (datamax_pfs_mode != backup_pfs_mode)
	{
		fprintf(stderr,
				"datamax and backup storage modes must match (got datamax=%s, backup=%s);\n"
				"mixed pfs/local mode is not supported in this port\n",
				datamax_pfs_mode ? "pfs" : "local",
				backup_pfs_mode ? "pfs" : "local");
		goto end;
	}

	/*
	 * init fs for datamax dir, use polar_disk_name_backup if
	 * polar_disk_name_datamax not specified
	 */
	if (polar_disk_name_datamax == NULL)
		polar_disk_name_datamax = polar_disk_name_backup;
	polar_enable_shared_storage_mode = datamax_pfs_mode;
	polar_vfs_init_fe(datamax_pfs_mode, "datamax", polar_storage_cluster_name, polar_disk_name_datamax, POLAR_VFS_RDWR);
	init_datamax_fs = true;

	/*
	 * only init backup fs when polar_disk_name_backup is not the same as
	 * polar_disk_name_datamax
	 */
	if (polar_disk_name_backup && polar_disk_name_datamax && (strcmp(polar_disk_name_backup, polar_disk_name_datamax) != 0))
	{
		polar_vfs_init_fe(backup_pfs_mode, "backup", polar_storage_cluster_name, polar_disk_name_backup, POLAR_VFS_RDWR);
		init_backup_fs = true;
	}

	if (datamax_datadir == NULL)
	{
		fprintf(stderr, "datamax data directory is NULL, need to be specified validly\n");
		goto end;
	}
	if (polar_stat(datamax_datadir, &stat_buf) != 0)
	{
		fprintf(stderr, "datamax_datadir doesn't exist\n");
		goto end;
	}

	if (backup_waldir == NULL)
	{
		fprintf(stderr, "backup wal directory is NULL, need to be specified validly\n");
		goto end;
	}
	if (polar_stat(backup_waldir, &stat_buf) != 0)
	{
		fprintf(stderr, "backup wal directory doesn't exist\n");
		goto end;
	}

	/* validate polar_datamax dir */
	if (polar_validate_datamax_dir(datamax_datadir) != 0)
	{
		fprintf(stderr, "validate polar_datamax dir error\n");
		goto end;
	}

	/* get wal segment size from pg_control file */
	snprintf(datamax_control_filepath, MAXPGPATH, "%s/global/pg_control", datamax_datadir);
	wal_segment_size = polar_get_wal_segment_size(datamax_control_filepath);
	/* verify wal segment size */
	if (!IsValidWalSegSize(wal_segment_size))
	{
		fprintf(stderr, _("WAL segment size must be a power of two between 1 MB and 1 GB, but the control file specifies %d byte\n"), wal_segment_size);
		goto end;
	}

	/* copy wal file from backup_waldir to datamax_waldir */
	if (polar_datamax_copy_wal_from_backup(backup_waldir, datamax_datadir) == 0)
		succeed = true;

end:
	if (init_datamax_fs)
		polar_vfs_destroy_fe("datamax", polar_disk_name_datamax);
	if (init_backup_fs)
		polar_vfs_destroy_fe("backup", polar_disk_name_backup);
	return succeed ? 0 : -1;
}
