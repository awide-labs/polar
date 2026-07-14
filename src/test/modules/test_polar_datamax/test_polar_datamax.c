
#include "postgres.h"

#include "access/timeline.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "common/fe_memutils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "polar_datamax/polar_datamax.h"
#include "port.h"
#include "postmaster/startup.h"
#include "replication/walreceiver.h"
#include "replication/slot.h"
#include "storage/fd.h"
#include "storage/polar_fd.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/palloc.h"
#include <unistd.h>

PG_MODULE_MAGIC;
static void
test_polar_datamax_shmem_size()
{
	polar_local_node_type = POLAR_STANDALONE_DATAMAX;
	Assert(polar_datamax_shmem_size());

	polar_local_node_type = POLAR_PRIMARY;
	Assert(!polar_datamax_shmem_size());
}

/* this test needs to be called before others to init polar_datamax_ctl */
static void
test_polar_datamax_shmem_init()
{
	polar_set_node_type(POLAR_PRIMARY);
	polar_datamax_shmem_init();

	if (polar_datamax_ctl == NULL)
	{
		polar_set_node_type(POLAR_STANDALONE_DATAMAX);
		IsUnderPostmaster = false;
		Assert(!IsUnderPostmaster);
		polar_datamax_shmem_init();
		Assert(polar_datamax_ctl);

		IsUnderPostmaster = true;
		Assert(IsUnderPostmaster);
		polar_datamax_shmem_init();
	}

	polar_set_node_type(POLAR_PRIMARY);
	IsUnderPostmaster = true;
}

static void
test_polar_datamax_load_write_meta()
{
	char		meta_path[MAXPGPATH];
	TimeLineID	tli = 0;
	XLogRecPtr	lsn = 0;

	polar_enable_shared_storage_mode = true;
	polar_local_node_type = POLAR_STANDALONE_DATAMAX;
	polar_datamax_validate_dir();

	/* init in memory */
	polar_datamax_init_meta(&polar_datamax_ctl->meta, false);

	/* create meta */
	polar_datamax_write_meta(polar_datamax_ctl, false);
	snprintf(meta_path, MAXPGPATH, "%s/%s/%s", POLAR_DATA_DIR(), POLAR_DATAMAX_DIR, POLAR_DATAMAX_META_FILE);
	Assert(PathNameOpenFile(meta_path, O_RDONLY | PG_BINARY) > 0);

	/* update meta and write */
	polar_datamax_update_min_received_info(polar_datamax_ctl, 1, 2);
	polar_datamax_write_meta(polar_datamax_ctl, true);

	/* load and verify */
	polar_datamax_update_min_received_info(polar_datamax_ctl, 1, 1);
	polar_datamax_load_meta(polar_datamax_ctl);
	LWLockAcquire(&polar_datamax_ctl->meta_lock, LW_SHARED);
	tli = polar_datamax_ctl->meta.min_timeline_id;
	lsn = polar_datamax_ctl->meta.min_received_lsn;
	LWLockRelease(&polar_datamax_ctl->meta_lock);
	Assert(tli == 1);
	Assert(lsn == 2);

	/* delete meta file created */
	durable_unlink(meta_path, LOG);
	polar_local_node_type = POLAR_PRIMARY;
}

static void
test_polar_datamax_is_initial()
{
	polar_set_node_type(POLAR_STANDALONE_DATAMAX);
	/* case #1 */
	polar_datamax_init_meta(POLAR_DATAMAX_META, true);
	Assert(polar_datamax_is_initial(polar_datamax_ctl));

	/* case #2 */
	polar_datamax_update_received_info(polar_datamax_ctl, 1, InvalidXLogRecPtr);
	Assert(!polar_datamax_is_initial(polar_datamax_ctl));

	/* case #3 */
	polar_datamax_update_received_info(polar_datamax_ctl, POLAR_INVALID_TIMELINE_ID, 1);
	Assert(!polar_datamax_is_initial(polar_datamax_ctl));

	/* case #4 */
	polar_datamax_update_received_info(polar_datamax_ctl, 1, 1);
	Assert(!polar_datamax_is_initial(polar_datamax_ctl));

	/* case #5 */
	polar_set_node_type(POLAR_PRIMARY);
	Assert(!polar_datamax_is_initial(polar_datamax_ctl));
}

static void
test_polar_datamax_init_meta()
{
	pg_crc32c	crc;

	/* case #1 */
	polar_datamax_init_meta(POLAR_DATAMAX_META, false);
	Assert(POLAR_DATAMAX_META->magic == 0);
	Assert(POLAR_DATAMAX_META->version == 0);

	/* case #2 */
	polar_datamax_init_meta(POLAR_DATAMAX_META, true);
	Assert(POLAR_DATAMAX_META->magic == POLAR_DATAMAX_MAGIC);
	Assert(POLAR_DATAMAX_META->version == POLAR_DATAMAX_VERSION);
	Assert(POLAR_DATAMAX_META->min_timeline_id == POLAR_INVALID_TIMELINE_ID);
	Assert(POLAR_DATAMAX_META->min_received_lsn == InvalidXLogRecPtr);
	Assert(POLAR_DATAMAX_META->last_timeline_id == POLAR_INVALID_TIMELINE_ID);
	Assert(POLAR_DATAMAX_META->last_received_lsn == InvalidXLogRecPtr);

	crc = POLAR_DATAMAX_META->crc;
	POLAR_DATAMAX_META->crc = 0;
	POLAR_DATAMAX_META->crc = polar_datamax_calc_meta_crc((unsigned char *) POLAR_DATAMAX_META, sizeof(polar_datamax_meta_data_t));
	Assert(POLAR_DATAMAX_META->crc == crc);
}

static void
test_polar_datamax_update_received_info()
{
	TimeLineID	tli = 0;
	XLogRecPtr	lsn = 0;

	polar_datamax_update_received_info(polar_datamax_ctl, 2, 2);

	/* test polar_datamax_get_last_received_info */
	polar_datamax_get_last_received_info(polar_datamax_ctl, &tli, &lsn);
	Assert(tli == 2);
	Assert(lsn == 2);

	/* test polar_datamax_get_last_received_lsn */
	polar_datamax_update_received_info(polar_datamax_ctl, 1, 3);
	lsn = polar_datamax_get_last_received_lsn(polar_datamax_ctl, &tli);
	Assert(tli == 1);
	Assert(lsn == 3);
}

static void
test_polar_datamax_update_min_received_info()
{
	TimeLineID	tli = 0;
	XLogRecPtr	lsn = 0;

	polar_datamax_update_min_received_info(polar_datamax_ctl, 3, 3);
	LWLockAcquire(&polar_datamax_ctl->meta_lock, LW_SHARED);
	tli = polar_datamax_ctl->meta.min_timeline_id;
	lsn = polar_datamax_ctl->meta.min_received_lsn;
	LWLockRelease(&polar_datamax_ctl->meta_lock);
	Assert(tli == 3);
	Assert(lsn == 3);
}

static void
test_polar_datamax_handle_timeline_switch()
{
	char		path_history[MAXPGPATH];
	char		meta_path[MAXPGPATH];
	FILE	   *out_file;
	char	   *content = "1   0/A000078  no recovery target specified\n";
	TimeLineID	next_tli = 2;
	XLogRecPtr	cur_lsn = (XLogRecPtr) 0xA000020;

	/* case #1, no timeline switch */
	polar_set_node_type(POLAR_STANDALONE_DATAMAX);
	polar_local_node_type = POLAR_STANDALONE_DATAMAX;
	polar_datamax_update_received_info(polar_datamax_ctl, 1, cur_lsn);
	polar_datamax_handle_timeline_switch(polar_datamax_ctl);
	Assert(polar_is_datamax_mode);

	/* create 2.history file, polar_is_datamax_mode = true */
	polar_datamax_validate_dir();
	TLHistoryFilePath(path_history, next_tli);

	if ((out_file = fopen(path_history, "w")) == NULL)
	{
		elog(LOG, "could not open file \"%s\"", path_history);
		return;
	}

	if (fputs(content, out_file) < 0)
		elog(LOG, "could not write file content :%s", content);

	if (fclose(out_file))
		elog(LOG, "could not close file \"%s\"", path_history);

	/* create meta file */
	polar_datamax_init_meta(&polar_datamax_ctl->meta, true);
	polar_datamax_write_meta(polar_datamax_ctl, false);

	/* case #2, timeline switch */
	cur_lsn = (XLogRecPtr) 0xB000020;
	polar_datamax_update_received_info(polar_datamax_ctl, 1, cur_lsn);
	polar_datamax_handle_timeline_switch(polar_datamax_ctl);
	next_tli = 1;
	polar_datamax_get_last_received_info(polar_datamax_ctl, &next_tli, &cur_lsn);
	Assert(next_tli == 2);

	/* delete meta file */
	snprintf(meta_path, MAXPGPATH, "%s/%s/%s", POLAR_DATA_DIR(), POLAR_DATAMAX_DIR, POLAR_DATAMAX_META_FILE);
	durable_unlink(meta_path, LOG);

	/* delete history file */
	durable_unlink(path_history, LOG);

	polar_set_node_type(POLAR_PRIMARY);
	polar_local_node_type = POLAR_PRIMARY;
}

static void
test_polar_datamax_validate_dir()
{
	const char *dirs[] =
	{
		POLAR_DATAMAX_DIR,
		POLAR_DATAMAX_WAL_DIR,
		POLAR_DATAMAX_WAL_DIR "/archive_status"
	};
	char		path[MAXPGPATH];
	int			i = 0;

	polar_local_node_type = POLAR_STANDALONE_DATAMAX;
	polar_datamax_validate_dir();

	for (i = lengthof(dirs) - 1; i >= 0; i--)
	{
		struct stat stat_buf;

		snprintf((path), MAXPGPATH, "%s/%s", polar_datadir, dirs[i]);
		/* check and detele dir */
		Assert(polar_stat(path, &stat_buf) == 0 && S_ISDIR(stat_buf.st_mode));

		if (polar_rmdir(path) < 0)
			elog(LOG, "could not delete directory \"%s\"", path);
	}
	polar_local_node_type = POLAR_PRIMARY;
}


static void
test_create_wal_or_ready_file(XLogSegNo _log_seg_no, TimeLineID tli, int wal_file_num, bool wal_file)
{
	int			count = 0;
	char		path[MAXPGPATH];
	char		xlogfile[MAXPGPATH];
	int			fd;

	polar_local_node_type = POLAR_STANDALONE_DATAMAX;
	polar_datamax_validate_dir();

	while (count < wal_file_num)
	{
		count++;
		polar_is_datamax_mode = true;

		if (wal_file)
			XLogFilePath(path, tli, _log_seg_no, wal_segment_size);
		/* create xlog.ready file */
		else
		{
			XLogFileName(xlogfile, tli, _log_seg_no, wal_segment_size);
			polar_datamax_status_file_path(path, xlogfile, ".ready");
		}

		if ((fd = PathNameOpenFile(path, O_RDONLY | PG_BINARY)) < 0)
		{
			elog(LOG, "try to create file \"%s\"", path);
			fd = PathNameOpenFile(path, O_RDWR | O_CREAT | PG_BINARY);

			if (fd < 0)
				elog(LOG, "could not create file \"%s\"", path);
		}
		_log_seg_no++;
	}
	polar_local_node_type = POLAR_PRIMARY;
}

static void
test_delete_wal_file(XLogSegNo _log_seg_no, TimeLineID tli, int wal_file_num)
{
	int			count = 0;
	char		seg_name[MAXPGPATH];

	while (count < wal_file_num)
	{
		count++;
		XLogFileName(seg_name, tli, _log_seg_no, wal_segment_size);
		polar_datamax_remove_wal_file(seg_name);
		_log_seg_no++;
	}
}

static void
test_polar_datamax_remove_old_wal()
{
	TimeLineID	tli = 0;
	XLogRecPtr	lsn = 0;
	XLogSegNo	seg = 1;
	XLogRecPtr	reserved_lsn = InvalidXLogRecPtr;
	int			wal_file_num = 5;

	polar_datamax_update_min_received_info(polar_datamax_ctl, 3, 3);

	/*
	 * Materialize the meta file: the timeline-switch test deleted it, and
	 * polar_datamax_remove_old_wal() (also reached by the clean-task, archive
	 * and parse-xlog tests below) persists the meta without O_CREAT, which is
	 * a FATAL on a missing file.
	 */
	polar_datamax_write_meta(polar_datamax_ctl, false);

	/* case 1, not datamax mode */
	polar_set_node_type(POLAR_PRIMARY);
	polar_datamax_remove_old_wal(1, false);
	LWLockAcquire(&polar_datamax_ctl->meta_lock, LW_SHARED);
	tli = polar_datamax_ctl->meta.min_timeline_id;
	lsn = polar_datamax_ctl->meta.min_received_lsn;
	LWLockRelease(&polar_datamax_ctl->meta_lock);
	Assert(tli == 3);
	Assert(lsn == 3);

	/* case 2, invalid reserved_lsn */
	polar_set_node_type(POLAR_STANDALONE_DATAMAX);
	polar_datamax_remove_old_wal(InvalidXLogRecPtr, false);
	LWLockAcquire(&polar_datamax_ctl->meta_lock, LW_SHARED);
	tli = polar_datamax_ctl->meta.min_timeline_id;
	lsn = polar_datamax_ctl->meta.min_received_lsn;
	LWLockRelease(&polar_datamax_ctl->meta_lock);
	Assert(tli == 3);
	Assert(lsn == 3);

	/* case 3 force delete old wal */
	tli = 1;
	test_create_wal_or_ready_file(seg, tli, wal_file_num, true);
	seg = 3;
	XLogSegNoOffsetToRecPtr(seg, 0, wal_segment_size, reserved_lsn);
	polar_datamax_remove_old_wal(reserved_lsn, true);
	LWLockAcquire(&polar_datamax_ctl->meta_lock, LW_SHARED);
	lsn = polar_datamax_ctl->meta.min_received_lsn;
	LWLockRelease(&polar_datamax_ctl->meta_lock);
	Assert(lsn <= reserved_lsn);
	test_delete_wal_file(seg, tli, wal_file_num - seg + 1);
	polar_set_node_type(POLAR_PRIMARY);
}

static void
test_polar_datamax_reset_clean_task()
{
	polar_datamax_clean_task_t task;

	polar_datamax_reset_clean_task(polar_datamax_ctl);

	task.reserved_lsn = 1;
	task.force = true;

	SpinLockAcquire(&polar_datamax_ctl->lock);
	task = polar_datamax_ctl->clean_task;
	SpinLockRelease(&polar_datamax_ctl->lock);
	Assert(task.reserved_lsn == InvalidXLogRecPtr);
	Assert(task.force == false);
}

static void
test_polar_datamax_set_clean_task()
{
	polar_datamax_clean_task_t task;

	task.reserved_lsn = InvalidXLogRecPtr;
	task.force = false;

	polar_datamax_reset_clean_task(polar_datamax_ctl);
	Assert(polar_datamax_set_clean_task(1, true));

	SpinLockAcquire(&polar_datamax_ctl->lock);
	task = polar_datamax_ctl->clean_task;
	SpinLockRelease(&polar_datamax_ctl->lock);
	Assert(task.reserved_lsn == 1);
	Assert(task.force == true);

	Assert(!polar_datamax_set_clean_task(2, false));
	polar_datamax_reset_clean_task(polar_datamax_ctl);
}

static void
test_polar_datamax_handle_clean_task()
{
	XLogSegNo	seg = 1;
	TimeLineID	tli = 1;
	int			wal_file_num = 3;
	XLogRecPtr	reserved_lsn = InvalidXLogRecPtr;
	polar_datamax_clean_task_t task = {1, true};

	/* reset task */
	seg = 3;
	test_create_wal_or_ready_file(seg, tli, wal_file_num, true);
	polar_datamax_reset_clean_task(polar_datamax_ctl);
	XLogSegNoOffsetToRecPtr(seg, 0, wal_segment_size, reserved_lsn);

	/* set and handle task */
	polar_datamax_set_clean_task(reserved_lsn, true);
	polar_datamax_handle_clean_task(polar_datamax_ctl);

	/* after handle, judge if reset */
	SpinLockAcquire(&polar_datamax_ctl->lock);
	task = polar_datamax_ctl->clean_task;
	SpinLockRelease(&polar_datamax_ctl->lock);
	Assert(task.reserved_lsn == InvalidXLogRecPtr);
	Assert(task.force == false);

	/* delete wal file created */
	test_delete_wal_file(seg, tli, wal_file_num - seg + 1);
}

static void
test_polar_datamax_replication_start_lsn()
{
	ReplicationSlot *slot = palloc(sizeof(ReplicationSlot));
	XLogRecPtr	lsn = InvalidXLogRecPtr;
	XLogRecPtr	rel_lsn = InvalidXLogRecPtr;

	XLogSegNoOffsetToRecPtr(1, 5, wal_segment_size, lsn);
	XLogSegNoOffsetToRecPtr(1, 0, wal_segment_size, rel_lsn);

	if (slot)
	{
		SpinLockInit(&slot->mutex);
		SpinLockAcquire(&slot->mutex);
		slot->data.restart_lsn = lsn;
		SpinLockRelease(&slot->mutex);

		Assert(polar_datamax_replication_start_lsn(slot) == rel_lsn);
		pfree(slot);
	}
}

static void
test_polar_datamax_archive_and_remove_archivedone()
{
	XLogSegNo	seg = 1;
	XLogSegNo	tmp_seg = 1;
	TimeLineID	tli = 1;
	int			wal_file_num = 5;
	int			i = 0;
	XLogRecPtr	reserved_lsn = InvalidXLogRecPtr;
	char		path[MAXPGPATH];
	struct stat stat_mode;

	polar_set_node_type(POLAR_STANDALONE_DATAMAX);
	wal_level = WAL_LEVEL_REPLICA;
	XLogArchiveMode = ARCHIVE_MODE_ALWAYS;

	/* test polar_datamax_archive() */
	/* first test archive, don't enter loop */
	polar_datamax_archive();
	polar_datamax_archive_timeout = 1000;
	sleep(3);

	/* second, actual achive */
	test_create_wal_or_ready_file(seg, tli, wal_file_num, true);
	test_create_wal_or_ready_file(seg, tli, wal_file_num, false);
	polar_datamax_archive();
	/* test end */

	/* test polar_datamax_remove_archivedone_wal() */
	/* first remove archivedone wal, don't do actual work */
	polar_datamax_remove_archivedone_wal(polar_datamax_ctl);

	/* second, with wal_keep_size_mb large enough to cover wal_file_num */
	seg = wal_file_num;
	/* update last_remove_segno of upstream */
	polar_datamax_update_upstream_last_removed_segno(polar_datamax_ctl, seg);
	XLogSegNoOffsetToRecPtr(seg, 0, wal_segment_size, reserved_lsn);
	polar_datamax_update_received_info(polar_datamax_ctl, tli, reserved_lsn);
	/* compute in 64 bits: with 1 GB segments the product overflows int */
	wal_keep_size_mb = (int) ((uint64) wal_file_num * wal_segment_size / (1024 * 1024));
	polar_datamax_remove_archivedone_wal_timeout = 1000;
	sleep(3);
	polar_datamax_remove_archivedone_wal(polar_datamax_ctl);

	for (i = 0; i < wal_file_num; i++)
	{
		XLogFilePath(path, tli, tmp_seg, wal_segment_size);
		Assert(polar_stat(path, &stat_mode) == 0);
	}

	/* third, with wal_keep_size_mb = 0 */
	wal_keep_size_mb = 0;
	polar_datamax_remove_archivedone_wal_timeout = 1000;
	sleep(3);
	polar_datamax_remove_archivedone_wal(polar_datamax_ctl);

	for (i = 0; i < wal_file_num - 1; i++)
	{
		XLogFilePath(path, tli, tmp_seg, wal_segment_size);
		Assert(polar_stat(path, &stat_mode) < 0);
	}
	/* test end */

	/* delete wal file created */
	test_delete_wal_file(seg, tli, wal_file_num - seg + 1);
	polar_set_node_type(POLAR_PRIMARY);
}

static void
test_polar_datamax_save_replication_slots()
{
	/* no datamax mode */
	polar_set_node_type(POLAR_PRIMARY);
	polar_datamax_save_replication_slots();

	/* datamax mode, no actual save */
	polar_set_node_type(POLAR_STANDALONE_DATAMAX);
	polar_datamax_save_replication_slots();

	/* datamax mode, do actual save */
	polar_datamax_save_replication_slots_timeout = 1000;
	sleep(3);
	polar_datamax_save_replication_slots();

	polar_set_node_type(POLAR_PRIMARY);
}

static void
test_polar_datamax_prealloc_wal_file()
{
	XLogSegNo	seg = 1;
	TimeLineID	tli = 1;
	int			wal_file_num = 1;
	int			i = 0;
	XLogRecPtr	reserved_lsn = InvalidXLogRecPtr;
	char		path[MAXPGPATH];
	struct stat stat_mode;
	WalRcvData *walrcv = NULL;
	WalRcvState state;

	/* no datamax mode */
	polar_set_node_type(POLAR_PRIMARY);
	polar_datamax_prealloc_wal_file(polar_datamax_ctl);

	/* datamax mode, no wal streaming */
	polar_set_node_type(POLAR_STANDALONE_DATAMAX);
	walrcv = WalRcv;
	state = WALRCV_STOPPING;
	SpinLockAcquire(&walrcv->mutex);
	walrcv->walRcvState = state;
	SpinLockRelease(&walrcv->mutex);
	polar_datamax_prealloc_wal_file(polar_datamax_ctl);

	/* datamax mode, wal streaming */
	state = WALRCV_STREAMING;
	SpinLockAcquire(&walrcv->mutex);
	walrcv->walRcvState = state;
	SpinLockRelease(&walrcv->mutex);

	test_create_wal_or_ready_file(seg, tli, wal_file_num, true);
	XLogSegNoOffsetToRecPtr(seg, 1, wal_segment_size, reserved_lsn);
	polar_datamax_update_received_info(polar_datamax_ctl, tli, reserved_lsn);
	polar_datamax_prealloc_walfile_timeout = 1000;
	sleep(3);
	polar_is_datamax_mode = true;
	polar_datamax_prealloc_wal_file(polar_datamax_ctl);

	for (i = 1; i <= polar_datamax_prealloc_walfile_num; i++)
	{
		XLogFilePath(path, tli, seg + i, wal_segment_size);
		Assert(polar_stat(path, &stat_mode) == 0);
	}

	/* wal file have been preallocated before */
	sleep(3);
	polar_datamax_prealloc_wal_file(polar_datamax_ctl);

	test_delete_wal_file(seg, tli, wal_file_num + polar_datamax_prealloc_walfile_num);
	polar_set_node_type(POLAR_PRIMARY);
}

static void
test_polar_datamax_create_walfile(bool modify_wal, unsigned long modify_start,
								  const char *readpath)
{
#define TEST_WAL_SIZE 16*1024*1024
	char		path[MAXPGPATH];
	File		fd;
	FILE	   *readfile;
	int			nbytes;
	char	   *buffer,
			   *start;
	XLogSegNo	segno;
	TimeLineID	tli;

	/* create wal file 000000010000000000000001 in polar_datamax/pg_wal dir */
	segno = 1;
	tli = 1;
	test_create_wal_or_ready_file(segno, tli, 1, true);

	/* file allocate */
	XLogFilePath(path, tli, segno, wal_segment_size);
	if ((fd = PathNameOpenFile(path, O_RDWR | PG_BINARY)) < 0)
		elog(ERROR, "could not open file \"%s\"", path);
	if (FileFallocate(fd, 0, wal_segment_size, WAIT_EVENT_WAL_INIT_WRITE) != 0)
		elog(ERROR, "FileFallocate file \"%s\" failed", path);

	/* read from test walfile */
	if ((readfile = fopen(readpath, "rb")) == NULL)
	{
		elog(ERROR, "could not open file 000000010000000000000001 for read ");
		return;
	}
	rewind(readfile);
	buffer = (char *) malloc(sizeof(char) * TEST_WAL_SIZE);
	if (buffer == NULL)
	{
		elog(ERROR, "allocate memory for buffer error");
		return;
	}
	if (fread(buffer, 1, TEST_WAL_SIZE, readfile) != TEST_WAL_SIZE)
	{
		elog(ERROR, "read file 000000010000000000000001 failed");
		return;
	}
	fclose(readfile);

	/* modify the last byte to make xlog checksum error */
	if (modify_wal)
		memset(buffer + modify_start, 0, TEST_WAL_SIZE - modify_start);

	/*
	 * copy 16M from test walfile to polar_datamax/pg_wal/walfile, and fill 0
	 * for remain space
	 */
	for (nbytes = 0; nbytes < wal_segment_size; nbytes += XLOG_BLCKSZ)
	{
		errno = 0;
		if (nbytes < TEST_WAL_SIZE)
			start = buffer + nbytes;
		else
		{
			memset(buffer, 0, sizeof(char) * TEST_WAL_SIZE);
			start = buffer;
		}
		if ((int) FileWrite(fd, start, XLOG_BLCKSZ, (off_t) nbytes,
							WAIT_EVENT_WAL_INIT_WRITE) != (int) XLOG_BLCKSZ)
		{
			int			save_errno = errno;

			FileClose(fd);
			polar_unlink(path);
			/* if write didn't set errno, assume problem is no disk space */
			errno = save_errno ? save_errno : ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", path)));
		}
	}
	if (FileSync(fd, WAIT_EVENT_WAL_INIT_SYNC) != 0)
	{
		int			save_errno = errno;

		FileClose(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	}
	FileClose(fd);

	free(buffer);
}

static void
test_polar_datamax_parse_xlog(const char *readpath)
{
	TimeLineID	tli;
	XLogRecPtr	received_lsn = InvalidXLogRecPtr;
	XLogRecPtr	valid_lsn = InvalidXLogRecPtr;
	XLogRecPtr	last_valid_lsn = InvalidXLogRecPtr;
	unsigned long modify_start = 0;

	/* case 1 received_lsn and valid_lsn are both invalid */
	polar_enable_shared_storage_mode = true;
	polar_local_node_type = POLAR_STANDALONE_DATAMAX;
	polar_datamax_validate_dir();
	polar_datamax_init_meta(&polar_datamax_ctl->meta, false);
	polar_datamax_parse_xlog(polar_datamax_ctl);

	/*
	 * case 2 xlog is correct, parse xlog.
	 *
	 * These LSNs are derived from the checked-in fixture
	 * 000000010000000000000001 (a real 16 MB-segment WAL image, so the file
	 * is one whole segment with base LSN 0x1000000): valid_lsn is the end of
	 * the segment's first record, received_lsn a record boundary near the end
	 * of the segment. They must be regenerated together with the fixture
	 * whenever the WAL format changes (XLOG_PAGE_MAGIC bump) -- see README.
	 */
	tli = 1;
	received_lsn = 0x1FFFBB8;
	valid_lsn = 0x10000A0;
	polar_datamax_update_received_info(polar_datamax_ctl, tli, received_lsn);
	polar_datamax_update_last_valid_received_lsn(polar_datamax_ctl, valid_lsn);
	test_polar_datamax_create_walfile(false, 0, readpath);
	polar_datamax_parse_xlog(polar_datamax_ctl);

	/* valid_lsn will be updated after parsing xlog */
	valid_lsn = polar_datamax_get_last_valid_received_lsn(polar_datamax_ctl, &tli);
	Assert(received_lsn == valid_lsn);

	/* case 3 modify xlog */
	received_lsn = 0x1FFFBB8;
	valid_lsn = 0x10000A0;
	last_valid_lsn = 0x1FFF400;
	modify_start = 16774144;	/* = 0xFFF400, in-segment offset of
								 * last_valid_lsn */
	polar_datamax_init_meta(&polar_datamax_ctl->meta, false);
	polar_datamax_update_received_info(polar_datamax_ctl, tli, received_lsn);
	polar_datamax_update_last_valid_received_lsn(polar_datamax_ctl, valid_lsn);
	test_polar_datamax_create_walfile(true, modify_start, readpath);
	polar_datamax_parse_xlog(polar_datamax_ctl);

	/* valid_lsn will be updated after parsing xlog */
	valid_lsn = polar_datamax_get_last_valid_received_lsn(polar_datamax_ctl, NULL);
	Assert(valid_lsn == last_valid_lsn);

	polar_local_node_type = POLAR_PRIMARY;
}

static void
test_polar_datamax_valid_lsn_list_operation()
{
	XLogRecPtr	valid_lsn = InvalidXLogRecPtr;
	polar_datamax_valid_lsn_list *polar_datamax_received_valid_lsn_list = NULL;

	/* case 1 init valid lsn list */
	polar_datamax_received_valid_lsn_list = polar_datamax_create_valid_lsn_list();

	/* case 2 insert new valid lsn */
	polar_datamax_insert_last_valid_lsn(polar_datamax_received_valid_lsn_list, (uint64) (2));
	/* insert duplicate value */
	polar_datamax_insert_last_valid_lsn(polar_datamax_received_valid_lsn_list, (uint64) (2));
	polar_datamax_insert_last_valid_lsn(polar_datamax_received_valid_lsn_list, (uint64) (4));
	polar_datamax_insert_last_valid_lsn(polar_datamax_received_valid_lsn_list, (uint64) (6));
	Assert(polar_datamax_received_valid_lsn_list->list_length == 3);

	/* insert when reach max value */
	polar_datamax_received_valid_lsn_list->list_length = 200;
	polar_datamax_insert_last_valid_lsn(polar_datamax_received_valid_lsn_list, (uint64) (8));
	Assert(polar_datamax_received_valid_lsn_list->list_length == 200);

	/* case 3 update meta.valid_lsn */
	polar_enable_shared_storage_mode = true;
	polar_local_node_type = POLAR_STANDALONE_DATAMAX;
	polar_datamax_validate_dir();
	polar_datamax_init_meta(&polar_datamax_ctl->meta, false);

	/* update when list length is 0 */
	polar_datamax_received_valid_lsn_list->list_length = 0;
	polar_datamax_update_cur_valid_lsn(polar_datamax_received_valid_lsn_list, (uint64) (1));
	valid_lsn = polar_datamax_get_last_valid_received_lsn(polar_datamax_ctl, NULL);
	Assert(valid_lsn == InvalidXLogRecPtr);

	/* update when flush_lsn < smaller lsn */
	polar_datamax_received_valid_lsn_list->list_length = 3;
	polar_datamax_update_cur_valid_lsn(polar_datamax_received_valid_lsn_list, (uint64) (1));
	Assert(polar_datamax_received_valid_lsn_list->list_length == 3);
	valid_lsn = polar_datamax_get_last_valid_received_lsn(polar_datamax_ctl, NULL);
	Assert(valid_lsn == InvalidXLogRecPtr);

	/* update valid lsn */
	polar_datamax_update_cur_valid_lsn(polar_datamax_received_valid_lsn_list, (uint64) (5));
	Assert(polar_datamax_received_valid_lsn_list->list_length == 1);
	valid_lsn = polar_datamax_get_last_valid_received_lsn(polar_datamax_ctl, NULL);
	Assert(valid_lsn == 4);

	/* free memory of valid_lsn_list */
	polar_datamax_free_valid_lsn_list(0, PointerGetDatum(polar_datamax_received_valid_lsn_list));

	polar_local_node_type = POLAR_PRIMARY;

}


PG_FUNCTION_INFO_V1(test_polar_datamax);
/*
 * SQL-callable entry point to perform all tests.
 */
Datum
test_polar_datamax(PG_FUNCTION_ARGS)
{
	char	   *readpath = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!polar_enable_shared_storage_mode)
		PG_RETURN_VOID();

	elog(LOG, "start of test case.");

	test_polar_datamax_shmem_size();
	test_polar_datamax_shmem_init();
	test_polar_datamax_validate_dir();
	test_polar_datamax_is_initial();
	test_polar_datamax_init_meta();
	test_polar_datamax_load_write_meta();
	test_polar_datamax_update_received_info();
	test_polar_datamax_update_min_received_info();
	test_polar_datamax_handle_timeline_switch();
	test_polar_datamax_remove_old_wal();
	test_polar_datamax_reset_clean_task();
	test_polar_datamax_set_clean_task();
	test_polar_datamax_handle_clean_task();
	test_polar_datamax_replication_start_lsn();
	test_polar_datamax_archive_and_remove_archivedone();
	test_polar_datamax_save_replication_slots();
	test_polar_datamax_prealloc_wal_file();
	test_polar_datamax_parse_xlog(readpath);
	test_polar_datamax_valid_lsn_list_operation();

	/* detele datamax dirs created for test */
	test_polar_datamax_validate_dir();

	PG_RETURN_VOID();
}
