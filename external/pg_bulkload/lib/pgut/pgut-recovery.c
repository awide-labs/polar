/*
 * pg_bulkload: lib/pgut/pgut-recovery.c
 *
 *	  Copyright (c) 2007-2026, NTT, Inc.
 *
 * Loader recovery logic shared by the pg_bulkload frontend and the
 * pg_bulkload extension (for postmaster/backend use).
 */

#ifdef FRONTEND
#include "postgres_fe.h"
#include "polar_vfs/polar_vfs_fe.h"
#include "pgut/pgut.h"
#else
#include "postgres.h"
#include "storage/polar_fd.h"
#endif

#include <signal.h>
#include <time.h>

#include "pgut-recovery.h"
#include "pg_loadstatus.h"

#include "catalog/pg_control.h"
#include "catalog/pg_tablespace.h"
#include "nodes/pg_list.h"

/**
 * @brief length of ".loadstatus" file
 * (Used to search files whose names end with ".loadStatus".)
 */
#define LSFEXT 11

/* Data directory for the current recovery run (set by StartLoaderRecovery). */
static const char *recovery_data_dir = NULL;

static List *GetLSFList(void);
static DBState GetDBClusterState(const char *fname);
static void GetLoadStatusInfo(const char *lsfpath, LoadStatus * ls);
static void TruncateLoadedRange(
#if PG_VERSION_NUM >= 160000
			RelFileLocator rLocator,
#else
			RelFileNode rnode,
#endif
			BlockNumber blkbeg, BlockNumber blkend);
static void GetSegmentPath(char path[MAXPGPATH],
#if PG_VERSION_NUM >= 160000
		RelFileLocator rLocator,
#else
		RelFileNode rnode,
#endif
		int segno);

static void
GetSegmentPath(char path[MAXPGPATH],
#if PG_VERSION_NUM >= 160000
		RelFileLocator rLocator,
#else
		RelFileNode rnode,
#endif
		int segno)
{
	char		relpath[MAXPGPATH];

#if PG_VERSION_NUM >= 160000
	if (rLocator.spcOid == GLOBALTABLESPACE_OID)
#else
	if (rnode.spcNode == GLOBALTABLESPACE_OID)
#endif
	{
		/* Shared system relations live in {datadir}/global */
#if PG_VERSION_NUM >= 160000
		snprintf(relpath, MAXPGPATH, "global/%u", rLocator.relNumber);
#else
		snprintf(relpath, MAXPGPATH, "global/%u", rnode.relNode);
#endif
	}
#if PG_VERSION_NUM >= 160000
	else if (rLocator.spcOid == DEFAULTTABLESPACE_OID)
#else
	else if (rnode.spcNode == DEFAULTTABLESPACE_OID)
#endif

	{
		/* The default tablespace is {datadir}/base */
#if PG_VERSION_NUM >= 160000
		snprintf(relpath, MAXPGPATH, "base/%u/%u", rLocator.dbOid, rLocator.relNumber);
#else
		snprintf(relpath, MAXPGPATH, "base/%u/%u", rnode.dbNode, rnode.relNode);
#endif
	}
	else
	{
		/* All other tablespaces are accessed via symlinks */
#if PG_VERSION_NUM >= 160000
		snprintf(relpath, MAXPGPATH, "pg_tblspc/%u/%u/%u", rLocator.spcOid, rLocator.dbOid, rLocator.relNumber);
#else
		snprintf(relpath, MAXPGPATH, "pg_tblspc/%u/%u/%u", rnode.spcNode, rnode.dbNode, rnode.relNode);
#endif
	}

	if (segno > 0)
	{
		size_t	len = strlen(relpath);

		snprintf(relpath + len, MAXPGPATH - len, ".%u", segno);
	}

	polar_make_file_path_level2(path, relpath);
}

static List *
GetLSFList(void)
{
	char	   *tmp;
	int			i,
				filelen;
	struct dirent *dp;
	List	   *list = NIL;
	DIR		   *dir;
	char		lsf_dir[MAXPGPATH];

	/*
	 * verify path of $PGDATA is not NULL
	 */
	Assert(recovery_data_dir != NULL);

	/*
	 * Build the path to the LSF directory on shared storage (or DataDir in
	 * non-shared-storage mode), then scan it for ".loadstatus" files.
	 */
	polar_make_file_path_level2(lsf_dir, BULKLOAD_LSF_DIR);

	if ((dir = polar_opendir(lsf_dir)) == NULL)
		return NIL;

	while ((dp = polar_readdir(dir)) != NULL)
	{
		tmp = dp->d_name;
		filelen = strlen(dp->d_name);

		if (filelen > LSFEXT)
		{
			for (i = 0; i < (filelen - LSFEXT); i++)
				tmp++;

			if ((strcmp(tmp, ".loadstatus") == 0))
				list = lappend(list, pstrdup(dp->d_name));
		}
	}

	if (polar_closedir(dir) == -1)
		elog(ERROR,
			 "could not close LSF Directory \"%s\": %m",
			 lsf_dir);

	return list;
}

static DBState
GetDBClusterState(const char *fname)
{
	int				fd;
	ControlFileData ControlFile;

	/*
	 * confirm path of $PGDATA is not NULL
	 */
	Assert(recovery_data_dir != NULL);

	/*
	 * open, read, and close ControlFileData
	 */
	if ((fd = polar_open(fname, O_RDONLY | PG_BINARY, 0)) == -1)
		elog(ERROR,
			 "could not open control file \"%s\": %m",
			 fname);

	if (polar_read(fd, &ControlFile, sizeof(ControlFile)) != sizeof(ControlFile))
		elog(ERROR,
			 "could not read control file \"%s\": %m",
			 fname);

	/* TODO: check CRC of the control file here. */

	if (polar_close(fd) == -1)
		elog(ERROR,
			 "could not close control file \"%s\": %m",
			 fname);

	return ControlFile.state;
}

static void
GetLoadStatusInfo(const char *lsfpath, LoadStatus * ls)
{
	int			fd;
	int			read_len;

	Assert(lsfpath != NULL);

	/*
	 * open and read LSF
	 */
	if ((fd = polar_open(lsfpath, O_RDONLY | PG_BINARY, 0)) == -1)
		elog(ERROR,
			 "could not open LoadStatusFile \"%s\": %m",
			 lsfpath);

	read_len = polar_read(fd, ls, sizeof(LoadStatus));
	if (read_len != sizeof(LoadStatus))
		elog(ERROR,
			 "could not read LoadStatusFile \"%s\": %m",
			 lsfpath);

	if (polar_close(fd) == -1)
		elog(ERROR,
			 "could not close LoadStatusFile \"%s\": %m",
			 lsfpath);
}

/**
 * @brief Discard a loader-extended block range by truncating the affected
 *        segment files on disk.
 *
 * pg_bulkload's DIRECT writer extends a relation under AccessExclusiveLock
 * and only WAL-logs the very first newly extended page (so that mdnblocks()
 * can see the new EOF after replay).  All later loader-written pages are
 * not WAL-logged.  After a crash before commit, those pages are physically
 * present on disk but reference an aborted XID; they must be removed
 * before the table is reachable again.
 *
 * @note Assumes blkbeg is at or before the pre-load EOF of its segment,
 *       so that truncating that segment to <tt>(blkbeg % RELSEG_SIZE) *
 *       BLCKSZ</tt> bytes preserves all pre-existing data.
 * @note Must be called before the relation is attached to shared buffers;
 *       StartLoaderRecovery() runs during startup, before WAL replay opens
 *       the relation and before any backend can touch it.
 * @note Idempotent across retries: a crash or a non-ENOENT polar_unlink()
 *       failure leaves the LSF on disk, so the next StartLoaderRecovery()
 *       run repeats the truncate (a no-op on an already-short segment) and
 *       the unlink loop (already-removed segments come back as ENOENT,
 *       which is tolerated explicitly).
 *
 * @warning The caller MUST keep the .loadstatus file on disk until this
 *          function returns without error.  If the LSF is unlinked while
 *          loader-created segments still exist on disk, those segments
 *          become invisible orphans that a later mdextend() can adopt as
 *          legitimate relation content, silently corrupting the table.
 *          See StartLoaderRecovery() for the enforced ordering.
 *
 * @param rLocator [in] (PG >= 16) physical locator of the relation to recover.
 * @param rnode    [in] (PG <  16) physical locator of the relation to recover.
 * @param blkbeg   [in] First block of the loader-extended range (inclusive),
 *                      typically <tt>ls.exist_cnt</tt>.
 * @param blkend   [in] One past the last loader-extended block (exclusive),
 *                      typically <tt>ls.exist_cnt + ls.create_cnt</tt>.
 *                      If <tt>blkbeg >= blkend</tt> this is a no-op.
 */
static void
TruncateLoadedRange(
#if PG_VERSION_NUM >= 160000
			RelFileLocator rLocator,
#else
			RelFileNode rnode,
#endif
			BlockNumber blkbeg, BlockNumber blkend)
{
	BlockNumber first_seg;
	BlockNumber last_seg;
	BlockNumber segno;
	off_t		first_seg_len;
	char		segpath[MAXPGPATH];

	/* Nothing to do if the loader did not extend the relation. */
	if (blkbeg >= blkend)
		return;

	first_seg = blkbeg / RELSEG_SIZE;
	last_seg = (blkend - 1) / RELSEG_SIZE;
	first_seg_len = (off_t) (blkbeg % RELSEG_SIZE) * BLCKSZ;

	/*
	 * Truncate the first affected segment back to the pre-load EOF.  If the
	 * loader started exactly on a segment boundary this trims it to zero
	 * length, but we keep the file in place: md.c requires every segment
	 * except the last to be exactly RELSEG_SIZE blocks, and the segment with
	 * index first_seg is allowed to be the (only) last segment of the
	 * relation after we are done.
	 */
	GetSegmentPath(segpath,
#if PG_VERSION_NUM >= 160000
				   rLocator,
#else
				   rnode,
#endif
				   first_seg);

	if (polar_truncate(segpath, first_seg_len) != 0)
		elog(ERROR,
			 "could not truncate data file \"%s\" to %lld bytes: %m",
			 segpath, (long long) first_seg_len);

	elog(NOTICE,
		 "truncated \"%s\" to %lld bytes (loader-extended range [%u, %u))",
		 segpath, (long long) first_seg_len, blkbeg, blkend);

	/*
	 * Unlink any later segments that the loader created.  These segments did
	 * not exist before the load (otherwise blkbeg would be past them), so it
	 * is safe to remove them entirely.
	 */
	for (segno = first_seg + 1; segno <= last_seg; segno++)
	{
		GetSegmentPath(segpath,
#if PG_VERSION_NUM >= 160000
					   rLocator,
#else
					   rnode,
#endif
					   segno);

		if (polar_unlink(segpath) != 0 && errno != ENOENT)
			elog(ERROR,
				 "could not unlink loader-created segment \"%s\": %m",
				 segpath);

		elog(NOTICE, "removed loader-created segment \"%s\"", segpath);
	}
}

void
StartLoaderRecovery(const char *data_dir)
{
	List	   *lsflist = NULL;
	ListCell   *cur;
	LoadStatus	ls;
	bool		need_recovery;
	char		pgcontrol_path[MAXPGPATH];

	/*
	 * verify DataDir
	 */
	Assert(data_dir != NULL);

	recovery_data_dir = data_dir;

	/*
	 * verify existence of load status file.
	 * need to free lsflist later.
	 */
	lsflist = GetLSFList();

	/*
	 * if lsflist is empty, need not to recovery by loader
	 */
	if (list_length(lsflist) == 0)
	{
		recovery_data_dir = NULL;
		return;
	}

	polar_make_file_path_level2(pgcontrol_path, "global/pg_control");
	need_recovery = GetDBClusterState(pgcontrol_path) != DB_SHUTDOWNED;

	/*
	 * while there are load status files, process recovery.
	 */
	foreach(cur, lsflist)
	{
		char	   *lsfname;
		char		lsfpath[MAXPGPATH];
		char		lsf_relpath[MAXPGPATH];

		lsfname = (char *) lfirst(cur);

		snprintf(lsf_relpath, MAXPGPATH, BULKLOAD_LSF_DIR "/%s", lsfname);
		polar_make_file_path_level2(lsfpath, lsf_relpath);

		/*
		 * if database cluster has abnormally shutdown,
		 * start recovery of overwriting blank pages.
		 */
		if (need_recovery)
		{
			/*
			 * get contents of load status file.
			 */
			GetLoadStatusInfo(lsfpath, &ls);

			/*
			 * XXX :need to store relaion name?
			 */
			elog(NOTICE,
				 "Starting pg_bulkload recovery for file \"%s\"",
				 lsfname);

			/*
			 * Discard pages created by the loader by truncating the affected
			 * segments back to their pre-load EOF and unlinking any segments
			 * that the loader newly created.
			 */
			TruncateLoadedRange(
#if PG_VERSION_NUM >= 160000
								ls.ls.rLocator,
#else
								ls.ls.rnode,
#endif
								ls.ls.exist_cnt,
								ls.ls.exist_cnt + ls.ls.create_cnt);

			elog(NOTICE,
				 "Ended pg_bulkload recovery for file \"%s\"",
				 lsfname);
		}

		/*
		 * delete load status file.
		 */
		if (polar_unlink(lsfpath) != 0)
			elog(ERROR,
				 "could not delete loadstatus file \"%s\": %m",
				 lsfpath);

		elog(NOTICE, "delete loadstatus file \"%s\"", lsfname);
	}

	list_free_deep(lsflist);
	recovery_data_dir = NULL;

	/* revocery process succeeded */
	elog(NOTICE, "recovered all relations");
}
