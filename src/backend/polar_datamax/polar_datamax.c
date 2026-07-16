/*-------------------------------------------------------------------------
 *
 * polar_datamax.c
 *
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
 *    src/backend/polar_datamax/polar_datamax.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/timeline.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "polar_datamax/polar_datamax.h"
#include "postmaster/pgarch.h"
#include "postmaster/startup.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "storage/fd.h"
#include "storage/polar_fd.h"
#include "storage/pmsignal.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

/*
 * POLAR_DATA_DIR() lives in miscadmin.h; no local override.
 *
 * Wait events for datamax I/O are not yet first-class entries; alias to
 * existing IO/recovery wait events until they are promoted.
 */
/*
 * The meta-file IO aliases pending Phase 3.12 WaitEventIO promotion; the
 * activity main wait event is now first-class (WaitEventActivity).
 */
#define WAIT_EVENT_DATAMAX_META_READ	WAIT_EVENT_DATA_FILE_READ
#define WAIT_EVENT_DATAMAX_META_WRITE	WAIT_EVENT_DATA_FILE_WRITE

/* initial meta means just initilized meta */
#define DATAMAX_IS_INITIAL_META(meta) \
	((meta)->last_timeline_id == POLAR_INVALID_TIMELINE_ID && \
	 XLogRecPtrIsInvalid((meta)->last_received_lsn))

/*
 * We use mechanism which depend on polar_vfs extension to maintain DataMax mode.
 * polar_vfs extension will initialize node type from recovery.conf while extension
 * initialization in polar_vfs_shmem_startup. So we relay on polar_vfs and its
 * startup function being executed while shared memory initialization.
 *
 * So the DataMax mode will be set before all other operations in DB, and its value
 * wil be kept while postmaster fork another sub-process. Eventually, all processes
 * know the DataMax mode.
 */
bool		polar_datamax_mode_requested = false;
bool		polar_is_datamax_mode = false;
polar_datamax_ctl_t *polar_datamax_ctl = NULL;

/* POLAR: set true when received shutdown request (written from SIGTERM handler) */
volatile sig_atomic_t polar_datamax_shutdown_requested = false;

/* POLAR: polar datamax guc parameters */
int			polar_datamax_mode = POLAR_DATAMAX_OFF;
int			polar_datamax_remove_archivedone_wal_timeout = 60000;
int			polar_datamax_archive_timeout = 60000;
int			polar_datamax_save_replication_slots_timeout = 300000;
int			polar_datamax_prealloc_walfile_timeout = 30000;
int			polar_datamax_prealloc_walfile_num = 2;

static char *primary_conninfo = NULL;
static char *primary_slot_name = NULL;
static int	polar_datamax_io_errno;
static polar_datamax_io_error_cause polar_datamax_io_errcause;
static void polar_datamax_xlog_read(TimeLineID tli, XLogRecPtr start_ptr, char *buf, Size count);
static int	polar_datamax_xlog_read_page(XLogReaderState *state, XLogRecPtr target_page_ptr, int req_len,
										 XLogRecPtr target_ptr, char *read_buff);

/*
 * POLAR: func for test
 */
char *
polar_datamax_get_primary_info(void)
{
	return primary_conninfo;
}

char *
polar_datamax_get_primary_slot_name(void)
{
	return primary_slot_name;
}

/*
 * POLAR: datamax shared memory size.
 */
Size
polar_datamax_shmem_size(void)
{
	Size		size = 0;

	/*
	 * POLAR: use polar_local_node_type in case XlogCtl can't be accessed
	 * during reaper process
	 */
	if (polar_local_node_type != POLAR_STANDALONE_DATAMAX)
		return size;

	size = add_size(size, sizeof(polar_datamax_ctl_t));

	return size;
}

/*
 * POLAR: datamax shared memory initialization.
 */
void
polar_datamax_shmem_init(void)
{
	bool		found = false;

	if (!polar_is_datamax())
		return;

	polar_datamax_ctl = (polar_datamax_ctl_t *)
		ShmemInitStruct("Datamax control", sizeof(polar_datamax_ctl_t), &found);

	if (!IsUnderPostmaster)
	{
		int			lock_tranche = LWLockNewTrancheId();

		Assert(!found);
		LWLockRegisterTranche(lock_tranche, "datamax meta lock");
		LWLockInitialize(&polar_datamax_ctl->meta_lock, lock_tranche);
		polar_datamax_init_meta(POLAR_DATAMAX_META, false);

		SpinLockInit(&polar_datamax_ctl->lock);
		polar_datamax_reset_clean_task(polar_datamax_ctl);
	}
	else
		Assert(found);
}

/*
 * POLAR: Whether DataMax is a initial one or not.
 *
 * Return true if it's a initial DataMax node, otherwise false which means
 * it's not an initial DataMax node or not even a DataMax node.
 */
bool
polar_datamax_is_initial(polar_datamax_ctl_t *ctl)
{
	bool		ret = false;

	if (!polar_is_datamax())
		return false;

	if (ctl)
	{
		LWLockAcquire(&ctl->meta_lock, LW_SHARED);
		ret = DATAMAX_IS_INITIAL_META(&ctl->meta);
		LWLockRelease(&ctl->meta_lock);
		return ret;
	}
	else
		return false;
}

/*
 * POLAR: Init datamax meta data info.
 *
 * If create is true, we initialize all variables with some initial value.
 * Otherwise, if not we set all variables 0.
 */
void
polar_datamax_init_meta(polar_datamax_meta_data_t *meta, bool create)
{
	if (create)
	{
		meta->magic = POLAR_DATAMAX_MAGIC;
		meta->version = POLAR_DATAMAX_VERSION;
	}
	else
	{
		meta->magic = 0;
		meta->version = 0;
	}

	meta->min_timeline_id = POLAR_INVALID_TIMELINE_ID;
	meta->min_received_lsn = InvalidXLogRecPtr;
	meta->last_timeline_id = POLAR_INVALID_TIMELINE_ID;
	meta->last_received_lsn = InvalidXLogRecPtr;
	meta->last_valid_received_lsn = InvalidXLogRecPtr;
	meta->upstream_last_removed_segno = 0;
	meta->crc = 0;
	meta->crc = polar_datamax_calc_meta_crc(
											(unsigned char *) meta, sizeof(polar_datamax_meta_data_t));
}

/*
 * POLAR: Load datamax meta info from storage.
 * We will check magic, version and crc of meta from data corruption.
 */
void
polar_datamax_load_meta(polar_datamax_ctl_t *ctl)
{
	char		meta_path[MAXPGPATH];
	pg_crc32c	crc;
	File		fd;
	int			ret;
	Size		meta_size = sizeof(polar_datamax_meta_data_t);
	polar_datamax_meta_data_t *meta = NULL;

	Assert(ctl);
	meta = &ctl->meta;

	snprintf(meta_path, MAXPGPATH, "%s/%s/%s",
			 POLAR_DATA_DIR(), POLAR_DATAMAX_DIR, POLAR_DATAMAX_META_FILE);

	LWLockAcquire(&ctl->meta_lock, LW_EXCLUSIVE);

	if ((fd = PathNameOpenFile(meta_path, O_RDONLY | PG_BINARY)) < 0)
	{
		if (errno == ENOENT)
		{
			elog(WARNING, "datamax meta file not exist, init memory one.");
			polar_datamax_init_meta(&ctl->meta, true);
			LWLockRelease(&ctl->meta_lock);
			return;
		}

		LWLockRelease(&ctl->meta_lock);
		elog(FATAL, "fail to open datamax meta file: %m");

		return;
	}

	ret = FileRead(fd, (char *) meta, meta_size,
				   0, WAIT_EVENT_DATAMAX_META_READ);

	if (ret != meta_size)
	{
		int			saved_errno = errno;

		LWLockRelease(&ctl->meta_lock);
		elog(FATAL, "fail to read whole datamax meta data, read %d of %lu, errno: %d",
			 ret, meta_size, saved_errno);
		return;
	}

	if (meta->magic != POLAR_DATAMAX_MAGIC)
	{
		LWLockRelease(&ctl->meta_lock);
		POLAR_LOG_DATAMAX_META_INFO(meta);
		elog(FATAL, "invalid datamax meta magic, got %d, expect %d",
			 meta->magic, POLAR_DATAMAX_MAGIC);
		return;
	}

	/* version compatibility will be added if new version required */
	if (meta->version != POLAR_DATAMAX_VERSION)
	{
		LWLockRelease(&ctl->meta_lock);
		POLAR_LOG_DATAMAX_META_INFO(meta);
		elog(FATAL, "datamax meta version is not compatible, got %d, expect %d",
			 meta->version, POLAR_DATAMAX_VERSION);
		return;
	}

	crc = meta->crc;
	meta->crc = 0;
	meta->crc = polar_datamax_calc_meta_crc((unsigned char *) meta, meta_size);

	if (crc != meta->crc)
	{
		LWLockRelease(&ctl->meta_lock);
		POLAR_LOG_DATAMAX_META_INFO(meta);
		elog(FATAL, "invalid datamax meta crc, got %d, expect %d", crc, meta->crc);
		return;
	}

	FileClose(fd);

	LWLockRelease(&ctl->meta_lock);
}

/*
 * POLAR: Write datamax meta data to storage. Acquires meta_lock; both the
 * datamax daemon and the walreceiver process write the meta, so the lock is
 * required for cross-process consistency.
 */
void
polar_datamax_write_meta(polar_datamax_ctl_t *ctl, bool update)
{
	File		fd;
	char		meta_path[MAXPGPATH];
	int			flag = O_RDWR | PG_BINARY;
	Size		meta_size = sizeof(polar_datamax_meta_data_t);
	int			saved_errno;
	polar_datamax_meta_data_t *meta = NULL;

	if (ctl)
		meta = &ctl->meta;
	else
	{
		elog(FATAL, "fail to write datamax meta data, invalid datamax meta");
		return;
	}

	snprintf(meta_path, MAXPGPATH, "%s/%s/%s", POLAR_DATA_DIR(), POLAR_DATAMAX_DIR, POLAR_DATAMAX_META_FILE);

	if (!update)
		flag |= O_CREAT;

	LWLockAcquire(&ctl->meta_lock, LW_EXCLUSIVE);

	meta->magic = POLAR_DATAMAX_MAGIC;
	meta->version = POLAR_DATAMAX_VERSION;
	meta->crc = 0;
	meta->crc = polar_datamax_calc_meta_crc((unsigned char *) meta, meta_size);

	if ((fd = PathNameOpenFile(meta_path, flag)) < 0)
	{
		saved_errno = errno;

		LWLockRelease(&ctl->meta_lock);

		POLAR_LOG_DATAMAX_META_INFO(meta);
		elog(FATAL, "fail to open datamax meta file, errno: %d", saved_errno);
		return;
	}

	if (FileWrite(fd, (char *) meta, meta_size, 0,
				  WAIT_EVENT_DATAMAX_META_WRITE) != meta_size)
	{
		saved_errno = errno;

		LWLockRelease(&ctl->meta_lock);

		POLAR_LOG_DATAMAX_META_INFO(meta);
		elog(FATAL, "fail to write datamax meta data, errno: %d", saved_errno);
		return;
	}

	if (FileSync(fd, WAIT_EVENT_DATAMAX_META_WRITE) != 0)
	{
		saved_errno = errno;

		LWLockRelease(&ctl->meta_lock);

		POLAR_LOG_DATAMAX_META_INFO(meta);
		elog(FATAL, "fail to sync datamax meta data to storage, errno: %d", saved_errno);
		return;
	}

	FileClose(fd);

	LWLockRelease(&ctl->meta_lock);
}

void
polar_datamax_update_received_info(polar_datamax_ctl_t *ctl, TimeLineID tli, XLogRecPtr lsn)
{
	Assert(ctl);
	LWLockAcquire(&ctl->meta_lock, LW_EXCLUSIVE);

	if (tli != POLAR_INVALID_TIMELINE_ID)
		ctl->meta.last_timeline_id = tli;

	if (!XLogRecPtrIsInvalid(lsn))
		ctl->meta.last_received_lsn = lsn;

	LWLockRelease(&ctl->meta_lock);
}

void
polar_datamax_update_last_valid_received_lsn(polar_datamax_ctl_t *ctl, XLogRecPtr lsn)
{
	Assert(ctl);
	LWLockAcquire(&ctl->meta_lock, LW_EXCLUSIVE);

	if (XLogRecPtrIsInvalid(lsn) || lsn < ctl->meta.last_valid_received_lsn)
		elog(FATAL, "invalid lsn when update datamax meta, parameter lsn is expected not smaller than last_valid_lsn");

	if (lsn != ctl->meta.last_valid_received_lsn)
		ctl->meta.last_valid_received_lsn = lsn;

	LWLockRelease(&ctl->meta_lock);
}

void
polar_datamax_update_min_received_info(polar_datamax_ctl_t *ctl, TimeLineID tli, XLogRecPtr lsn)
{
	Assert(ctl);
	LWLockAcquire(&ctl->meta_lock, LW_EXCLUSIVE);

	if (tli != POLAR_INVALID_TIMELINE_ID)
		ctl->meta.min_timeline_id = tli;

	if (!XLogRecPtrIsInvalid(lsn))
		ctl->meta.min_received_lsn = lsn;

	LWLockRelease(&ctl->meta_lock);
}

void
polar_datamax_update_upstream_last_removed_segno(polar_datamax_ctl_t *ctl, XLogSegNo segno)
{
	Assert(ctl);
	LWLockAcquire(&ctl->meta_lock, LW_EXCLUSIVE);

	/*
	 * POLAR: parameter segno could be smaller than
	 * meta.upstream_last_removed_segno eg. it maybe 0 when primary restart
	 * and there is no wal file has been removed
	 */
	if (segno > ctl->meta.upstream_last_removed_segno)
		ctl->meta.upstream_last_removed_segno = segno;

	LWLockRelease(&ctl->meta_lock);
}

XLogRecPtr
polar_datamax_get_min_received_lsn(polar_datamax_ctl_t *ctl, TimeLineID *tli)
{
	XLogRecPtr	min_received_lsn = InvalidXLogRecPtr;

	Assert(ctl);

	LWLockAcquire(&ctl->meta_lock, LW_SHARED);

	if (tli)
		*tli = ctl->meta.min_timeline_id;

	min_received_lsn = ctl->meta.min_received_lsn;

	LWLockRelease(&ctl->meta_lock);

	return min_received_lsn;
}

void
polar_datamax_get_last_received_info(polar_datamax_ctl_t *ctl, TimeLineID *tli, XLogRecPtr *lsn)
{
	Assert(ctl);
	LWLockAcquire(&ctl->meta_lock, LW_SHARED);

	if (tli)
		*tli = ctl->meta.last_timeline_id;

	if (lsn)
		*lsn = ctl->meta.last_received_lsn;

	LWLockRelease(&ctl->meta_lock);
}

/*
 * POLAR: get last received lsn
 */
XLogRecPtr
polar_datamax_get_last_received_lsn(polar_datamax_ctl_t *ctl, TimeLineID *tli)
{
	XLogRecPtr	last_received_lsn = InvalidXLogRecPtr;

	Assert(ctl);
	LWLockAcquire(&ctl->meta_lock, LW_SHARED);

	if (tli)
		*tli = ctl->meta.last_timeline_id;

	last_received_lsn = ctl->meta.last_received_lsn;

	LWLockRelease(&ctl->meta_lock);

	return last_received_lsn;
}

/*
 * POLAR: get last valid received lsn
 */
XLogRecPtr
polar_datamax_get_last_valid_received_lsn(polar_datamax_ctl_t *ctl, TimeLineID *tli)
{
	XLogRecPtr	last_valid_received_lsn = InvalidXLogRecPtr;

	Assert(ctl);
	LWLockAcquire(&ctl->meta_lock, LW_SHARED);

	if (tli)
		*tli = ctl->meta.last_timeline_id;

	last_valid_received_lsn = ctl->meta.last_valid_received_lsn;

	LWLockRelease(&ctl->meta_lock);

	return last_valid_received_lsn;
}

/*
 * POLAR: get last removed segno of upstream
 */
XLogSegNo
polar_datamax_get_upstream_last_removed_segno(polar_datamax_ctl_t *ctl)
{
	XLogRecPtr	upstream_last_removed_segno = 0;

	Assert(ctl);
	LWLockAcquire(&ctl->meta_lock, LW_SHARED);
	upstream_last_removed_segno = ctl->meta.upstream_last_removed_segno;
	LWLockRelease(&ctl->meta_lock);
	return upstream_last_removed_segno;
}

/*
 * POLAR: handle timeline switch.
 *
 * If replication is done because of end-of-timeline, we will fetch all timeline history
 * from primary. So we can read timeline history to locate which timeline we belong now.
 * If timeline switch, update meta timeline which will be used while next try to request
 * stream replication.
 */
void
polar_datamax_handle_timeline_switch(polar_datamax_ctl_t *ctl)
{
	XLogRecPtr	cur_lsn = InvalidXLogRecPtr;
	TimeLineID	cur_tli = POLAR_INVALID_TIMELINE_ID;
	TimeLineID	latest_tli = POLAR_INVALID_TIMELINE_ID;
	TimeLineID	next_tli = POLAR_INVALID_TIMELINE_ID;
	List	   *expectedTLEs;

	cur_lsn = polar_datamax_get_last_received_lsn(ctl, &cur_tli);

	polar_is_datamax_mode = true;
	latest_tli = findNewestTimeLine(cur_tli);

	Assert(latest_tli >= cur_tli);

	elog(LOG, "latest timeline is %d, current timeline is %d", latest_tli, cur_tli);

	/* POLAR: if current timeline is latest, no need to switch timeline */
	if (latest_tli == cur_tli)
		return;

	expectedTLEs = readTimeLineHistory(cur_tli + 1);
	next_tli = tliOfPointInHistory(cur_lsn, expectedTLEs);
	Assert(next_tli >= cur_tli);

	/* POLAR: if timeline switch happened, update timeline of meta */
	if (next_tli > cur_tli)
	{
		polar_datamax_update_received_info(ctl, next_tli, InvalidXLogRecPtr);
		polar_datamax_write_meta(ctl, true);
		elog(LOG, "find new timeline to switch, switch from timeline %d to timeline %d at lsn %lX",
			 cur_tli, next_tli, cur_lsn);
	}
}

/*
 * POLAR: try to remove old wal.
 *
 * lsn is not involved in removed segment.
 */
void
polar_datamax_remove_old_wal(XLogRecPtr reserved_lsn, bool force)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		path[MAXPGPATH];
	char		last_seg[MAXPGPATH];
	XLogRecPtr	min_lsn = InvalidXLogRecPtr;
	XLogSegNo	segno,
				upstream_last_removed_segno;
	TimeLineID	min_reserved_tli = POLAR_INVALID_TIMELINE_ID;
	XLogRecPtr	min_reserved_lsn = InvalidXLogRecPtr;

	if (!polar_is_datamax())
		return;

	/* parameter check */
	if (XLogRecPtrIsInvalid(reserved_lsn))
		return;

	/* calculate slot min lsn first */
	ReplicationSlotsComputeRequiredLSN();

	min_lsn = XLogGetReplicationSlotMinimumLSN();

	/* use smaller one between reserved_lsn and slot min restart_lsn */
	if (XLogRecPtrIsInvalid(min_lsn) || reserved_lsn < min_lsn)
		min_lsn = reserved_lsn;

	/* calculate last removed segment number */
	XLByteToSeg(min_lsn, segno, wal_segment_size);
	segno--;

	/* get last removed segno of upstream */
	upstream_last_removed_segno = polar_datamax_get_upstream_last_removed_segno(polar_datamax_ctl);
	/* keep wal which haven't been removed in upstream node */
	if (segno > upstream_last_removed_segno)
		segno = upstream_last_removed_segno;

	polar_make_file_path_level2(path, POLAR_DATAMAX_WAL_DIR);

	XLogFileName(last_seg, 0, segno, wal_segment_size);

	elog(LOG, "datamax attempting to remove WAL segment file older than log file %s",
		 last_seg);

	xldir = AllocateDir(path);

	while ((xlde = ReadDir(xldir, path)) != NULL)
	{
		bool		removed = false;

		if (!IsXLogFileName(xlde->d_name) &&
			!IsPartialXLogFileName(xlde->d_name))
			continue;

		/*
		 * only compare segment name without timeline info, detail reason is
		 * described at RemoveOldXlogFiles.
		 */
		if (strcmp(xlde->d_name + 8, last_seg + 8) <= 0)
		{
			/*
			 * if force is true or WAL segment has been archived, we will
			 * remove it.
			 */
			if (force || XLogArchiveCheckDone(xlde->d_name))
			{
				if (polar_datamax_remove_wal_file(xlde->d_name) == 0)
					removed = true;
			}
		}

		if (!removed)
		{
			uint32		tmp_tli;
			XLogSegNo	tmp_segno;
			XLogRecPtr	tmp_seg_lsn;

			/* try to find min received info */
			XLogFromFileName(xlde->d_name, &tmp_tli, &tmp_segno, wal_segment_size);
			XLogSegNoOffsetToRecPtr(tmp_segno, 0, wal_segment_size, tmp_seg_lsn);

			if (XLogRecPtrIsInvalid(min_reserved_lsn) || min_reserved_lsn > tmp_seg_lsn)
			{
				min_reserved_tli = tmp_tli;
				min_reserved_lsn = tmp_seg_lsn;
			}
		}
	}

	FreeDir(xldir);

	Assert(!XLogRecPtrIsInvalid(min_reserved_lsn) &&
		   min_reserved_tli != POLAR_INVALID_TIMELINE_ID);

	/* update min received info according to actual WAL file */
	polar_datamax_update_min_received_info(polar_datamax_ctl, min_reserved_tli, min_reserved_lsn);
	polar_datamax_write_meta(polar_datamax_ctl, true);
	elog(LOG, "remove useless WAL segment done, min_reserved_tli %d, min_received_lsn %lX",
		 min_reserved_tli, min_reserved_lsn);
}

/*
 * POLAR: Remove one WAL segment file.
 *
 * We will also remove status file related with current segment.
 */
int
polar_datamax_remove_wal_file(char *seg_name)
{
	int			rc = 0;
	char		seg_path[MAXPGPATH];

	UpdateLastRemovedPtr(seg_name);

	polar_make_file_path_level3(seg_path, POLAR_DATAMAX_WAL_DIR, seg_name);

	elog(LOG, "removing WAL segment file %s", seg_name);
	rc = durable_unlink(seg_path, LOG);

	if (rc != 0)
	{
		/* detail log was printed in durable_unlink */
		elog(WARNING, "remove WAL segment file failed.");
		return rc;
	}

	/* remove status files */
	XLogArchiveCleanup(seg_name);
	return rc;
}

/*
 * POLAR: handle WAL clean task
 */
void
polar_datamax_handle_clean_task(polar_datamax_ctl_t *ctl)
{
	polar_datamax_clean_task_t task = {InvalidXLogRecPtr, false};

	Assert(ctl);
	SpinLockAcquire(&ctl->lock);

	if (POLAR_DATAMAX_IS_VALID_CLEAN_TASK(&ctl->clean_task))
		task = ctl->clean_task;

	SpinLockRelease(&ctl->lock);

	/* no valid clean task, do nothing */
	if (!POLAR_DATAMAX_IS_VALID_CLEAN_TASK(&task))
		return;

	/* do clean task */
	polar_datamax_remove_old_wal(task.reserved_lsn, task.force);

	/* reset clean task */
	polar_datamax_reset_clean_task(ctl);
}

/*
 * POLAR: Set WAL clean task. Exposed to user as SQL interface.
 */
bool
polar_datamax_set_clean_task(XLogRecPtr reserved_lsn, bool force)
{
	polar_datamax_ctl_t *ctl = polar_datamax_ctl;

	if (!ctl)
		return false;

	SpinLockAcquire(&ctl->lock);

	if (POLAR_DATAMAX_IS_VALID_CLEAN_TASK(&ctl->clean_task))
	{
		SpinLockRelease(&ctl->lock);
		elog(LOG, "there is datamax clean task ongoing.");
		return false;
	}

	ctl->clean_task.reserved_lsn = reserved_lsn;
	ctl->clean_task.force = force;

	SpinLockRelease(&ctl->lock);

	return true;
}

/*
 * POLAR: reset clean task
 */
void
polar_datamax_reset_clean_task(polar_datamax_ctl_t *ctl)
{
	Assert(ctl);
	SpinLockAcquire(&ctl->lock);
	ctl->clean_task.reserved_lsn = InvalidXLogRecPtr;
	ctl->clean_task.force = false;
	SpinLockRelease(&ctl->lock);
}

/*
 * POLAR: archive wal file, need archive_mode == always
 */
void
polar_datamax_archive(void)
{
	static TimestampTz last_archive_time = 0;

	if (unlikely(last_archive_time == 0))
		last_archive_time = GetCurrentTimestamp();

	if (XLogArchivingAlways() && polar_is_datamax() && polar_datamax_archive_timeout)
	{
		TimestampTz archive_now = GetCurrentTimestamp();

		if (TimestampDifferenceExceeds(last_archive_time, archive_now, polar_datamax_archive_timeout))
		{
			polar_datamax_ArchiverCopyLoop();
			last_archive_time = archive_now;
		}
	}
}

/*
 * POLAR: remove wal files whose archive status are .done
 * if archive_mode = off or archive_mode = on, the .done file will be created forcibly in walreceiver.c
 * if archive_mode > on, the .done file will be created only when corresponding walfile has been archived
 * so there is no need to judge archive_mode value in this func
 */
void
polar_datamax_remove_archivedone_wal(polar_datamax_ctl_t *ctl)
{
	static TimestampTz last_remove_archivedone_wal_time = 0;

	if (unlikely(last_remove_archivedone_wal_time == 0))
		last_remove_archivedone_wal_time = GetCurrentTimestamp();

	/* only remove when timeout is set valid and in datamax mode */
	if (polar_is_datamax() && polar_datamax_remove_archivedone_wal_timeout)
	{
		TimestampTz now = GetCurrentTimestamp();

		if (TimestampDifferenceExceeds(last_remove_archivedone_wal_time, now, polar_datamax_remove_archivedone_wal_timeout))
		{
			polar_datamax_handle_remove_archivedone_wal(ctl);
			last_remove_archivedone_wal_time = now;
		}
	}
}

/*
 * POLAR: handle remove wal files whose archive status are .done
 * besides, don't remove wal files those haven't been removed in upstream instance
 * in case we lose walfile which is needed by downstream after re-create datamax node
 * which will be judged in func polar_datamax_remove_old_wal
 */
void
polar_datamax_handle_remove_archivedone_wal(polar_datamax_ctl_t *ctl)
{
	XLogRecPtr	last_rec_lsn = InvalidXLogRecPtr;
	XLogRecPtr	reserved_lsn = InvalidXLogRecPtr;
	XLogSegNo	segno;

	if (!polar_is_datamax())
		return;

	/* get last received lsn */
	last_rec_lsn = polar_datamax_get_last_received_lsn(ctl, NULL);

	/* return when received no wal */
	if (XLogRecPtrIsInvalid(last_rec_lsn))
		return;

	XLByteToSeg(last_rec_lsn, segno, wal_segment_size);

	/* compute limit for wal_keep_size_mb */
	if (wal_keep_size_mb > 0)
	{
		uint64		keep_segments;

		keep_segments = XLogMBVarToSegs(wal_keep_size_mb, wal_segment_size);
		if (segno <= keep_segments)
			segno = 1;
		else
			segno = segno - keep_segments;
	}

	XLogSegNoOffsetToRecPtr(segno, 0, wal_segment_size, reserved_lsn);
	polar_datamax_remove_old_wal(reserved_lsn, false);
}

/*
 * POLAR: Check DataMax directory, if directory is missing, we will create it.
 */
void
polar_datamax_validate_dir(void)
{
	char		path[MAXPGPATH];
	int			i = 0;
	const char *dirs[] =
	{
		POLAR_DATAMAX_DIR,
		POLAR_DATAMAX_WAL_DIR,
		POLAR_DATAMAX_WAL_DIR "/archive_status"
	};

	if (polar_local_node_type != POLAR_STANDALONE_DATAMAX)
		elog(FATAL, "Only validate datamax dir when in DataMax mode.");

	for (i = 0; i < lengthof(dirs); ++i)
	{
		polar_make_file_path_level2(path, dirs[i]);

		/* check and create dir */
		polar_datamax_check_mkdir(path, FATAL);
	}
}

/*
 * POLAR: Utility to check dir existence, if not create one.
 *
 * parameter path should be an absolute path.
 */
void
polar_datamax_check_mkdir(const char *path, int emode)
{
	struct stat stat_buf;

	if (polar_stat(path, &stat_buf) == 0)
	{
		if (!S_ISDIR(stat_buf.st_mode))
			elog(emode, "path \"%s\" is not a directory", path);
	}
	else
	{
		elog(LOG, "create missing directory \"%s\"", path);

		if (polar_make_pg_directory(path) < 0)
			elog(emode, "could not create directory \"%s\"", path);
	}
}

/*
 * POLAR: Return start pos of next WAL segment file after restart_lsn of slot.
 *
 * It is used while establishing initial replication from Datamax. Primary will
 * send WAL data as much as possible, and to avoid hole of WAL file in DataMax,
 * replication should begin at a start posistion of WAL file. That is why we
 * need this func.
 */
XLogRecPtr
polar_datamax_replication_start_lsn(ReplicationSlot *slot)
{
	XLogRecPtr	restart_lsn = InvalidXLogRecPtr;

	Assert(slot);
	SpinLockAcquire(&slot->mutex);
	restart_lsn = slot->data.restart_lsn;
	SpinLockRelease(&slot->mutex);

	Assert(!XLogRecPtrIsInvalid(restart_lsn));

	return restart_lsn - XLogSegmentOffset(restart_lsn, wal_segment_size);
}

/*
 * POLAR: iterate the waldir to get the lsn of smallest walfile
 * called when XLogGetLastRemovedSegno is 0
 */
XLogRecPtr
polar_get_smallest_walfile_lsn(void)
{
	XLogRecPtr	smallest_lsn,
				tmp_smallest_lsn;
	XLogSegNo	segno;
	char		xlog_file[MAXFNAMELEN];
	char		polar_waldir_path[MAXPGPATH];
	DIR		   *polar_waldir;
	struct dirent *polar_waldirent;
	uint32		tli;

	/* get the possibly smallest lsn */
	if (!polar_is_datamax())
		smallest_lsn = GetRedoRecPtr();
	else
		smallest_lsn = polar_datamax_get_min_received_lsn(polar_datamax_ctl, NULL);

	/* iterate the wal dir to get the smallest lsn */
	if (!polar_is_datamax())
		polar_make_file_path_level2(polar_waldir_path, XLOGDIR);
	else
		polar_make_file_path_level2(polar_waldir_path, POLAR_DATAMAX_WAL_DIR);

	polar_waldir = AllocateDir(polar_waldir_path);
	XLByteToSeg(smallest_lsn, segno, wal_segment_size);
	XLogFileName(xlog_file, 0, segno, wal_segment_size);
	while ((polar_waldirent = ReadDir(polar_waldir, polar_waldir_path)) != NULL)
	{
		if (!IsXLogFileName(polar_waldirent->d_name) &&
			!IsPartialXLogFileName(polar_waldirent->d_name))
			continue;
		if (strcmp(polar_waldirent->d_name + 8, xlog_file + 8) < 0)
		{
			/* found wal file smaller than xlog_file */
			XLogFromFileName(polar_waldirent->d_name, &tli, &segno, wal_segment_size);
			XLogSegNoOffsetToRecPtr(segno, 0, wal_segment_size, tmp_smallest_lsn);
			/* update restart_lsn when current wal file hasn't been removed */
			if (tmp_smallest_lsn < smallest_lsn)
				smallest_lsn = tmp_smallest_lsn;
		}
	}
	FreeDir(polar_waldir);
	return smallest_lsn;
}

/*
 * POLAR: save replication slots to pg_replslot
 * slots are saved by CreateCheckPoint/CreateRestartPoint when not in datamax mode
 * there is no checkpoint process in datamax mode
 * add this func to save replication slot in datamax mode
 */
void
polar_datamax_save_replication_slots(void)
{
	static TimestampTz last_saveslots_time = 0;

	if (unlikely(last_saveslots_time == 0))
		last_saveslots_time = GetCurrentTimestamp();

	if (polar_is_datamax() && polar_datamax_save_replication_slots_timeout)
	{
		TimestampTz saveslots_now = GetCurrentTimestamp();

		if (TimestampDifferenceExceeds(last_saveslots_time, saveslots_now, polar_datamax_save_replication_slots_timeout))
		{
			CheckPointReplicationSlots(false);
			last_saveslots_time = saveslots_now;
		}
	}
}

/*
 * POLAR: WAL file path for datamax
 */
void
polar_datamax_wal_file_path(char *path, TimeLineID tli, XLogSegNo logSegNo, int wal_segsz_bytes)
{
	if (polar_enable_shared_storage_mode)
	{
		snprintf(path, MAXPGPATH, "%s/" POLAR_DATAMAX_WAL_DIR "/%08X%08X%08X", polar_datadir, tli,
				 (uint32) ((logSegNo) / XLogSegmentsPerXLogId(wal_segsz_bytes)),
				 (uint32) ((logSegNo) % XLogSegmentsPerXLogId(wal_segsz_bytes)));
	}
	else
	{
		snprintf(path, MAXPGPATH, POLAR_DATAMAX_WAL_DIR "/%08X%08X%08X", tli,
				 (uint32) ((logSegNo) / XLogSegmentsPerXLogId(wal_segsz_bytes)),
				 (uint32) ((logSegNo) % XLogSegmentsPerXLogId(wal_segsz_bytes)));
	}
}

/*
 * POLAR: WAL status file path for DataMax
 */
void
polar_datamax_status_file_path(char *path, const char *xlog, char *suffix)
{
	if (polar_enable_shared_storage_mode)
		snprintf(path, MAXPGPATH, "%s/" POLAR_DATAMAX_WAL_DIR "/archive_status/%s%s", polar_datadir, xlog, suffix);
	else
		snprintf(path, MAXPGPATH, POLAR_DATAMAX_WAL_DIR "/archive_status/%s%s", xlog, suffix);
}

void
polar_datamax_tl_history_file_path(char *path, TimeLineID tli)
{
	if (polar_enable_shared_storage_mode)
		snprintf(path, MAXPGPATH, "%s/" POLAR_DATAMAX_WAL_DIR "/%08X.history", polar_datadir, tli);
	else
		snprintf(path, MAXPGPATH, POLAR_DATAMAX_WAL_DIR "/%08X.history", tli);
}

/*
 * POLAR: prealloc wal file
 * this work is done by polar worker process when not in datamax mode
 * add this func so it's unnecessary to start polar worker process in datamax mode
 */
void
polar_datamax_prealloc_wal_file(polar_datamax_ctl_t *ctl)
{
	static TimestampTz last_prealloc_walfile_time = 0;

	if (unlikely(last_prealloc_walfile_time == 0))
		last_prealloc_walfile_time = GetCurrentTimestamp();

	if (polar_is_datamax() && WalRcvStreaming() && polar_datamax_prealloc_walfile_timeout)
	{
		TimestampTz prealloc_walfile_now = GetCurrentTimestamp();

		if (TimestampDifferenceExceeds(last_prealloc_walfile_time, prealloc_walfile_now, polar_datamax_prealloc_walfile_timeout))
		{
			polar_datamax_handle_prealloc_walfile(ctl);
			last_prealloc_walfile_time = prealloc_walfile_now;
		}
	}
}

/*
 * POLAR: handle prealloc wal file
 *
 * Runs only in the datamax daemon, where polar_is_datamax_mode is already
 * pinned true by polar_datamax_main(), so XLogFileInit() resolves through
 * POLAR_DATAMAX_WAL_DIR without any local toggling.
 */
void
polar_datamax_handle_prealloc_walfile(polar_datamax_ctl_t *ctl)
{
	XLogRecPtr	insert_ptr;
	XLogSegNo	_log_seg_no;
	int			lf;
	int			count = 0;
	TimeLineID	tli = POLAR_INVALID_TIMELINE_ID;

	/*
	 * runs only in the datamax daemon, which pinned the flag in
	 * polar_datamax_main()
	 */
	Assert(polar_is_datamax_mode);

	insert_ptr = polar_datamax_get_last_received_lsn(ctl, &tli);

	if (!XLogRecPtrIsInvalid(insert_ptr))
	{
		XLByteToPrevSeg(insert_ptr, _log_seg_no, wal_segment_size);

		for (count = 0; count < polar_datamax_prealloc_walfile_num; count++)
		{
			/* XLogFileInit() takes (segno, tli) */
			lf = XLogFileInit(++_log_seg_no, tli);
			polar_close(lf);
		}
	}
}

/*
 * POLAR: judge whether meta file exists
 * no meta file when it is an initial created datamax
 */
bool
polar_datamax_meta_file_exist(void)
{
	char		meta_path[MAXPGPATH];
	struct stat stat_buf;

	snprintf(meta_path, MAXPGPATH, "%s/%s/%s", POLAR_DATA_DIR(), POLAR_DATAMAX_DIR, POLAR_DATAMAX_META_FILE);
	return (polar_stat(meta_path, &stat_buf) == 0);
}

/*
 * POLAR: create and init polar_datamax_valid_lsn_list
 */
polar_datamax_valid_lsn_list *
polar_datamax_create_valid_lsn_list(void)
{
	polar_datamax_valid_lsn_list *list = palloc(sizeof(polar_datamax_valid_lsn_list));

	dlist_init(&list->valid_lsn_lhead);
	list->list_length = 0;
	return list;
}

/*
 * POLAR: append new received primary_last_valid_lsn to list
 */
void
polar_datamax_insert_last_valid_lsn(polar_datamax_valid_lsn_list *list, XLogRecPtr primary_last_valid_lsn)
{
#define POLAR_VALIDLSN_LIST_MAX_LENGTH 200

	Assert(list);

	/* don't insert new node when reach the max length */
	if (list->list_length == POLAR_VALIDLSN_LIST_MAX_LENGTH)
		return;
	else
	{
		polar_datamax_valid_lsn *tail_valid_lsn;

		tail_valid_lsn = (list->list_length == 0) ? NULL : dlist_container(polar_datamax_valid_lsn, valid_lsn_lnode, dlist_tail_node(&list->valid_lsn_lhead));

		if (tail_valid_lsn && tail_valid_lsn->primary_valid_lsn > primary_last_valid_lsn)
			elog(FATAL, "invalid lsn when insert new node to datamax_valid_lsn_list");
		/* insert when list is empty or lsn is not duplicated */
		else if (tail_valid_lsn == NULL || tail_valid_lsn->primary_valid_lsn != primary_last_valid_lsn)
		{
			polar_datamax_valid_lsn *new_valid_lsn = palloc(sizeof(polar_datamax_valid_lsn));

			new_valid_lsn->primary_valid_lsn = primary_last_valid_lsn;
			dlist_push_tail(&list->valid_lsn_lhead, &new_valid_lsn->valid_lsn_lnode);
			list->list_length++;
		}
	}
}

/*
 * POLAR: update current last_valid_lsn when datamax received new wal
 * remove old valid lsn from list at the same time
 */
void
polar_datamax_update_cur_valid_lsn(polar_datamax_valid_lsn_list *list, XLogRecPtr flush_lsn)
{
	XLogRecPtr	cur_valid_lsn = InvalidXLogRecPtr;
	polar_datamax_valid_lsn *valid_lsn;
	dlist_mutable_iter diter;

	Assert(list);

	if (list->list_length == 0)
		return;

	/* get current valid lsn, the list is sorted */
	dlist_foreach_modify(diter, &list->valid_lsn_lhead)
	{
		valid_lsn = dlist_container(polar_datamax_valid_lsn, valid_lsn_lnode, diter.cur);

		if (valid_lsn->primary_valid_lsn <= flush_lsn)
		{
			cur_valid_lsn = valid_lsn->primary_valid_lsn;
			/* remove list node whose lsn is smaller than flush lsn */
			dlist_delete(&valid_lsn->valid_lsn_lnode);
			pfree(valid_lsn);
			list->list_length--;
		}
		else
			break;
	}

	/*
	 * update current valid lsn to meta, cur_valid_lsn is invalid when
	 * flush_lsn is smaller than head_lsn
	 */
	if (!XLogRecPtrIsInvalid(cur_valid_lsn))
		polar_datamax_update_last_valid_received_lsn(polar_datamax_ctl, cur_valid_lsn);
}

/*
 * POLAR: free memory of polar_datamax_received_valid_lsn_list
 * used as a before_shmem_exit handler
 */
void
polar_datamax_free_valid_lsn_list(int code, Datum arg)
{
	dlist_mutable_iter diter;
	polar_datamax_valid_lsn *cur_valid_lsn;

	polar_datamax_valid_lsn_list *list = (polar_datamax_valid_lsn_list *) DatumGetPointer(arg);

	if (list == NULL)
		return;
	else
	{
		dlist_foreach_modify(diter, &list->valid_lsn_lhead)
		{
			cur_valid_lsn = dlist_container(polar_datamax_valid_lsn, valid_lsn_lnode, diter.cur);
			dlist_delete(&cur_valid_lsn->valid_lsn_lnode);
			pfree(cur_valid_lsn);
			list->list_length--;
		}
	}

	/*
	 * check whether list->length is the same as the actual num of list
	 * members
	 */
	if (list->list_length != 0)
		elog(FATAL, "invalid datamax_valid_lsn_list, list->length is not the same as the actual num of list members\n");

	pfree(list);
}

/*
 * POLAR: read count bytes from a segment file, store the data in the passed buffer
 */
static void
polar_datamax_xlog_read(TimeLineID tli, XLogRecPtr start_ptr, char *buf, Size count)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes;
	char		path[MAXPGPATH];

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
			polar_datamax_wal_file_path(path, tli, read_segNo, wal_segment_size);

			if ((read_file = polar_open(path, O_RDONLY | PG_BINARY, 0)) == -1)
			{
				/* report error */
				polar_datamax_io_errcause = POLAR_DATAMAX_OPEN_FAILED;
				polar_datamax_io_errno = errno;
				polar_datamax_report_io_error(path, FATAL);
			}

			read_off = 0;
		}

		/* need to seek in the file */
		if (read_off != start_off)
		{
			if (polar_lseek(read_file, (off_t) start_off, SEEK_SET) < 0)
			{
				polar_datamax_wal_file_path(path, tli, read_segNo, wal_segment_size);
				/* report error */
				polar_datamax_io_errcause = POLAR_DATAMAX_SEEK_FAILED;
				polar_datamax_io_errno = errno;
				polar_datamax_report_io_error(path, FATAL);
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
			polar_datamax_wal_file_path(path, tli, read_segNo, wal_segment_size);
			/* report error */
			polar_datamax_io_errcause = POLAR_DATAMAX_READ_FAILED;
			polar_datamax_io_errno = errno;
			polar_datamax_report_io_error(path, FATAL);
		}

		/* update state for read */
		recptr += read_bytes;
		read_off += read_bytes;
		nbytes -= read_bytes;
		p += read_bytes;
	}
}

/*
 * POLAR: XLogReader read_page callback; ws_tli is set on the reader state
 * instead of a cur_file_tli out-parameter.
 */
static int
polar_datamax_xlog_read_page(XLogReaderState *state, XLogRecPtr target_page_ptr, int req_len,
							 XLogRecPtr target_ptr, char *read_buff)
{
	polar_datamax_xlog_parse_private *private = state->private_data;
	int			count = XLOG_BLCKSZ;

	if (target_page_ptr + XLOG_BLCKSZ <= private->end_ptr)
		count = XLOG_BLCKSZ;
	else if (target_page_ptr + req_len <= private->end_ptr)
		count = private->end_ptr - target_page_ptr;
	else
	{
		private->reach_end_ptr = true;
		return -1;
	}

	polar_datamax_xlog_read(private->timeline, target_page_ptr, read_buff, count);
	state->seg.ws_tli = private->timeline;
	return count;
}

/* No-op segment open/close: page_read manages its own files. */
static void
polar_datamax_xlog_segment_open(XLogReaderState *state, XLogSegNo nextSegNo,
								TimeLineID *tli_p)
{
}

static void
polar_datamax_xlog_segment_close(XLogReaderState *state)
{
}

/*
 * POLAR: parse xlog from last_valid_received_lsn in order to start xlog streaming from a valid lsn
 * so that we can prevent xlog inconsistency between primary and datamax
 * when the primary's last lsn is invalid, which will be covered when primary restart
 * but it won't be covered in datamax if we start xlog streaming from the last_received_lsn rather than last_valid_received_lsn
 */
void
polar_datamax_parse_xlog(polar_datamax_ctl_t *ctl)
{
	XLogReaderState *xlogreader;
	XLogReaderRoutine routine;
	polar_datamax_xlog_parse_private private;
	TimeLineID	tli;
	XLogRecPtr	last_valid_received_lsn;	/* start lsn of parsing */
	XLogRecPtr	last_received_lsn;	/* end lsn of parsing */
	XLogRecPtr	lsn_read_from;
	XLogRecord *record;
	char	   *errormsg;
	bool		new_valid = false;
	char		waldir[MAXPGPATH];

	polar_datamax_get_last_received_info(ctl, &tli, &last_received_lsn);
	last_valid_received_lsn = polar_datamax_get_last_valid_received_lsn(ctl, NULL);
	elog(LOG, "last_received_lsn:%X/%X, last_valid_received_lsn:%X/%X",
		 (uint32) (last_received_lsn >> 32), (uint32) last_received_lsn,
		 (uint32) (last_valid_received_lsn >> 32), (uint32) last_valid_received_lsn);

	/*
	 * nothing to do when last_valid_received_lsn or last_received_lsn is
	 * invalid which indicates it's an initial datamax node
	 */
	if (XLogRecPtrIsInvalid(last_received_lsn) || XLogRecPtrIsInvalid(last_valid_received_lsn))
		return;

	/*
	 * POLAR: if there is nothing to parse (initial bootstrap sets both
	 * endpoints to the same checkpoint position), skip.
	 */
	if (last_received_lsn == last_valid_received_lsn)
		return;

	Assert(last_received_lsn >= last_valid_received_lsn);

	/* set up xlog reader */
	MemSet(&private, 0, sizeof(polar_datamax_xlog_parse_private));
	private.start_ptr = last_valid_received_lsn;
	private.end_ptr = last_received_lsn;
	private.timeline = tli;
	private.reach_end_ptr = false;

	routine.page_read = polar_datamax_xlog_read_page;
	routine.segment_open = polar_datamax_xlog_segment_open;
	routine.segment_close = polar_datamax_xlog_segment_close;

	if (polar_enable_shared_storage_mode)
		snprintf(waldir, MAXPGPATH, "%s/%s",
				 polar_datadir, POLAR_DATAMAX_WAL_DIR);
	else
		snprintf(waldir, MAXPGPATH, "%s", POLAR_DATAMAX_WAL_DIR);

	xlogreader = XLogReaderAllocate(wal_segment_size, waldir, &routine, &private);

	if (!xlogreader)
		/* no cover begin */
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	/* no cover end */

	/*
	 * find a valid recptr to start from because the last_valid_received_lsn
	 * is the end of the last valid lsn of primary if the position is at a
	 * page boundary, it points to the beginning of the page,(ie. before page
	 * header) not to where the first xlog record on that page would go to
	 */
	XLogBeginRead(xlogreader, private.start_ptr);
	lsn_read_from = XLogFindNextRecord(xlogreader, private.start_ptr);

	if (lsn_read_from == InvalidXLogRecPtr)
	{
		elog(LOG, "could not find a valid record after %X/%X, which is the last valid lsn",
			 (uint32) (private.start_ptr >> 32),
			 (uint32) private.start_ptr);
		XLogReaderFree(xlogreader);
		return;
	}

	elog(LOG, "parse xlog record from:%X/%X", (uint32) (lsn_read_from >> 32), (uint32) lsn_read_from);

	/* parse xlog (XLogReadRecord takes state + errormsg) */
	XLogBeginRead(xlogreader, lsn_read_from);
	for (;;)
	{
		record = XLogReadRecord(xlogreader, &errormsg);

		/* record is reach the end of valid xlog record */
		if (!record || private.reach_end_ptr)
			break;

		/* new valid xlog record, update last_valid_received_lsn */
		last_valid_received_lsn = xlogreader->EndRecPtr;
		new_valid = true;
	}

	if (errormsg)
		elog(LOG, "error in WAl record at %X/%X: %s",
			 (uint32) (xlogreader->ReadRecPtr >> 32),
			 (uint32) xlogreader->ReadRecPtr, errormsg);

	XLogReaderFree(xlogreader);

	/* update last_valid_received_lsn when there is new valid record */
	if (new_valid)
	{
		polar_datamax_update_last_valid_received_lsn(ctl, last_valid_received_lsn);
		polar_datamax_write_meta(ctl, true);
	}

	elog(LOG, "last received valid record of primary is: %X/%X",
		 (uint32) (last_valid_received_lsn >> 32),
		 (uint32) last_valid_received_lsn);
}

/*
 * POLAR: report io error when parse xlog
 */
void
polar_datamax_report_io_error(const char *path, int log_level)
{
	errno = polar_datamax_io_errno;

	switch (polar_datamax_io_errcause)
	{
		case POLAR_DATAMAX_OPEN_FAILED:
			ereport(log_level,
					(errcode_for_file_access(),
					 errmsg("could not open wal file \"%s\" for reading: %m", path)));
			break;

		case POLAR_DATAMAX_SEEK_FAILED:
			ereport(log_level,
					(errcode_for_file_access(),
					 errmsg("could not seek in wal file \"%s\" : %m", path)));
			break;

		case POLAR_DATAMAX_READ_FAILED:
			ereport(log_level,
					(errcode_for_file_access(),
					 errmsg("could not read from wal file \"%s\" : %m", path)));
			break;

		default:
			elog(ERROR, "unrecognized polar datamax io error cause: %d", (int) polar_datamax_io_errcause);
			break;
	}
}

/*
 * POLAR: Main entry of DataMax node.
 * It will take some operations of Startup, like revoking stream replication,
 * config read. Also, it will do some DataMax operations, like config maintainment,
 * meta data managment, etc.
 */
void
polar_datamax_main(void)
{
	TimestampTz last_handle_interrupts_time = GetCurrentTimestamp();
	char		activitymsg[MAXFNAMELEN + 16];

	/*
	 * validate datamax directory. for shared storage, we keep meta data info
	 * in local storage, for standalone storage, we keep meta and wal data all
	 * in same storage.
	 */
	polar_datamax_validate_dir();

	/*
	 * POLAR: primary_conninfo and primary_slot_name are GUCs that GUC
	 * processing has already populated by the time we get here. Mirror them
	 * into the file-local statics that the rest of this module reads, and
	 * fail fast if the operator forgot to set them.
	 */
	polar_datamax_mode_requested = polar_is_datamax();
	primary_conninfo = PrimaryConnInfo;
	primary_slot_name = PrimarySlotName;

	if (!polar_datamax_mode_requested)
		ereport(FATAL,
				(errmsg("polar_datamax_main entered but datamax mode is not requested")));

	/*
	 * POLAR: we are now committed to running as the datamax daemon for the
	 * rest of this process's life. Route every WAL path resolution
	 * (XLogFileInit, XLogFilePath, archive_status, the archive copy loop,
	 * ...) through POLAR_DATAMAX_WAL_DIR by pinning polar_is_datamax_mode
	 * true once here. This process never serves non-datamax WAL, so the flag
	 * must never be reset; callees no longer toggle it.
	 */
	polar_is_datamax_mode = true;

	/*
	 * POLAR: primary_conninfo / primary_slot_name may not be set at first
	 * start: an operator may bring datamax up to run setup commands (CREATE
	 * EXTENSION, drop slot, etc.), then restart it with replication
	 * configured. Warn and rely on the main loop checking again before each
	 * RequestXLogStreaming().
	 */
	if (primary_conninfo == NULL || primary_conninfo[0] == '\0' ||
		primary_slot_name == NULL || primary_slot_name[0] == '\0')
		ereport(WARNING,
				(errmsg("datamax mode started without primary_conninfo / primary_slot_name; WAL streaming will be inactive until they are set and the node is restarted")));

	if (!IsUnderPostmaster)
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("datamax mode is not supported by single-user servers")));

	ereport(LOG, (errmsg("entering datamax mode")));

	/* load datamax meta info */
	polar_datamax_load_meta(polar_datamax_ctl);

	/* create meta file if it doesn't exist */
	if (!polar_datamax_meta_file_exist())
		polar_datamax_write_meta(polar_datamax_ctl, false);

	/*
	 * POLAR: a fresh datamax keeps its empty meta (last_timeline_id = 0,
	 * last_valid_received_lsn = invalid). It then requests streaming with an
	 * invalid timeline / lsn; the walreceiver adopts the primary's current
	 * timeline reported by IDENTIFY_SYSTEM, and the primary's walsender
	 * streams from the smallest WAL it still has (see
	 * polar_set_initial_datamax_restart_lsn). No checkpoint seed is needed,
	 * so a datamax can be provisioned by "initdb -i <sysid>" without copying
	 * the primary's data directory.
	 */

	/*
	 * init latestCompletedXid which is necessary while GetSnapshotData; it is
	 * a FullTransactionId.
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	TransamVariables->latestCompletedXid = TransamVariables->nextXid;
	FullTransactionIdRetreat(&TransamVariables->latestCompletedXid);
	LWLockRelease(ProcArrayLock);

	/*
	 * POLAR: allow XLogFileInit() to install new WAL segments. The flag is
	 * normally turned on after StartupXLOG finishes, but datamax skips the
	 * rest of startup.
	 */
	SetInstallXLogFileSegmentActive();

	/* notify postmaster turn into DATAMAX status */
	SendPostmasterSignal(PMSIGNAL_BEGIN_DATAMAX);

	/* update process title info */
	snprintf(activitymsg, sizeof(activitymsg), "polar_datamax");
	set_ps_display(activitymsg);

	/*
	 * parse xlog and update last_valid_received_lsn, which is used to request
	 * xlog streaming
	 */
	polar_datamax_parse_xlog(polar_datamax_ctl);

	/* Ready to go */
	while (true)
	{
		TimestampTz current_time = GetCurrentTimestamp();
		int			rc = 0;

		if (TimestampDifferenceExceeds(last_handle_interrupts_time, current_time, 5))
		{
			/* save replication slot info when shutdown */
			if (polar_datamax_shutdown_requested)
			{
				CheckPointReplicationSlots(true);

				/*
				 * POLAR: stop the walreceiver and exit cleanly so pg_ctl stop
				 * completes without timing out. Do not persist the meta here:
				 * the walreceiver already writes it on every flush, and an
				 * unconditional write would clobber an out-of-band meta
				 * update (e.g. "polar_tools datamax-get-wal" lowering
				 * min_received_lsn while the node runs) with the stale
				 * in-memory copy.
				 */
				ShutdownWalRcv();
				proc_exit(0);
			}

			/* Handle interrupt signals of startup process */
			HandleStartupProcInterrupts();
			/* POLAR: every 5 ms call this */
			last_handle_interrupts_time = current_time;
		}

		/*
		 * POLAR: re-read primary_conninfo / primary_slot_name from the GUC
		 * variables every iteration. HandleStartupProcInterrupts() above
		 * processes SIGHUP config reloads, and a reload frees and reallocates
		 * the GUC string storage. The values captured once at startup would
		 * then dangle, so a later RequestXLogStreaming() could request
		 * streaming with a garbage slot name. Refreshing here is a cheap
		 * pointer assignment and a no-op when nothing changed.
		 */
		primary_conninfo = PrimaryConnInfo;
		primary_slot_name = PrimarySlotName;

		/*
		 * Check whether walreceiver is streaming, if not, try to pull up
		 * walreceiver with suitable requested start position and timeline.
		 */
		if (!WalRcvStreaming())
		{
			/* ensure walreceiver is stopped */
			ShutdownWalRcv();

			/*
			 * POLAR: try to read timeline history of current timeline and
			 * compare endptr with current lsn. If current timeline is ended,
			 * we roll up current timeline and try to fetch wal from new
			 * timeline.
			 */
			polar_datamax_handle_timeline_switch(polar_datamax_ctl);

			if (primary_conninfo && primary_conninfo[0] != '\0' &&
				primary_slot_name && primary_slot_name[0] != '\0')
			{
				/*
				 * try to pull up walreceiver, start streaming from last valid
				 * lsn
				 */
				RequestXLogStreaming(
									 POLAR_DATAMAX_STREAMING_TIMELINE,
									 POLAR_DATAMAX_STREAMING_LSN,
									 primary_conninfo,
									 primary_slot_name,
									 false);

			}

			/*
			 * POLAR: keep the daemon alive even without primary settings; the
			 * operator may set them and reload.
			 */
		}

		polar_datamax_handle_clean_task(polar_datamax_ctl);

		/* save replication slots */
		polar_datamax_save_replication_slots();

		/* start archive */
		polar_datamax_archive();

		/* remove archive done wal */
		polar_datamax_remove_archivedone_wal(polar_datamax_ctl);

		/* prealloc wal file */
		polar_datamax_prealloc_wal_file(polar_datamax_ctl);

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   1 * 1000 /* ms */ , WAIT_EVENT_DATAMAX_MAIN);

		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);

		if (rc & WL_POSTMASTER_DEATH)
			exit(1);
	}
}
