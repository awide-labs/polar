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
#include "storage/bufpage.h"

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
static void ClearLoadedPage(
#if PG_VERSION_NUM >= 160000
			RelFileLocator rLocator,
#else
			RelFileNode rnode,
#endif
			BlockNumber blkbeg, BlockNumber blkend);
static bool IsPageCreatedByLoader(Page page);
static bool PageHeaderIsValid(Page page);
static void GetSegmentPath(char path[MAXPGPATH],
#if PG_VERSION_NUM >= 160000
		RelFileLocator rLocator,
#else
		RelFileNode rnode,
#endif
		int segno);

#ifdef FRONTEND
/*------------------------------------------------------------------------
 *	 The following function is copied from PostgreSQL source code with no
 *   changes. This is necessary only for frontend version, backend version
 *   uses PostgreSQL code.
 *------------------------------------------------------------------------*/

void
PageInit(Page page, Size pageSize, Size specialSize)
{
	PageHeader	p = (PageHeader) page;

	specialSize = MAXALIGN(specialSize);

	Assert(pageSize == BLCKSZ);
	Assert(pageSize > specialSize + SizeOfPageHeaderData);

	/*
	 * Make sure all fields of page are zero, as well as unused space
	 */
	MemSet(p, 0, pageSize);

	p->pd_lower = SizeOfPageHeaderData;
	p->pd_upper = pageSize - specialSize;
	p->pd_special = pageSize - specialSize;
	PageSetPageSizeAndVersion(page, pageSize, PG_PAGE_LAYOUT_VERSION);
}
#endif /* FRONTEND */

/*------------------------------------------------------------------------
 *   PageHeaderIsValid() is no longer exists in PostreSQL 15, the following
 *   function is the short version of PageIsVerifiedExtended(), used both
 *   for frontend and backend versions.
 *------------------------------------------------------------------------*/
bool
PageHeaderIsValid(Page page)
{
	char	   *pagebytes;
	int			i;
	PageHeader phdr = (PageHeader) page;

	/*
	 * Check normal case
	 */
	if (PageGetPageSize(
#if PG_VERSION_NUM >= 160000
		page) == BLCKSZ && PageGetPageLayoutVersion(page
#else
		phdr) == BLCKSZ && PageGetPageLayoutVersion(phdr
#endif
	 	) == PG_PAGE_LAYOUT_VERSION &&
		phdr->pd_lower >= SizeOfPageHeaderData &&
		phdr->pd_lower <= phdr->pd_upper &&
		phdr->pd_upper <= phdr->pd_special &&
		phdr->pd_special <= BLCKSZ &&
		phdr->pd_special == MAXALIGN(phdr->pd_special))
		return true;

	/*
	 * Check all-zeroes case
	 */
	pagebytes = (char *) phdr;
	for (i = 0; i < BLCKSZ; i++)
	{
		if (pagebytes[i] != 0)
			return false;
	}
	return true;
}

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

static bool
IsPageCreatedByLoader(Page page)
{
	PageHeader	targetBlock = (PageHeader) page;

	if (!PageHeaderIsValid(page))
		return true;

	if (targetBlock->pd_lsn.xlogid == 0 && targetBlock->pd_lsn.xrecoff == 0)
		return true;
	else
		return false;
}

static void
ClearLoadedPage(
#if PG_VERSION_NUM >= 160000
			RelFileLocator rLocator,
#else
			RelFileNode rnode,
#endif
			BlockNumber blkbeg, BlockNumber blkend)
{
	BlockNumber segno;				/* data file segment no */
	char		segpath[MAXPGPATH];	/* data file name to open */
	char	   *page;				/* area to read blocks */
	Page		zeropage;			/* blank page */
	BlockNumber	blknum;				/* block no currently procesing */
	int			fd;					/* file descriptor */
	off_t		seekpos;			/* position of block to recovery */
	ssize_t		ret;				/* return value of read()  */
	ssize_t		readlen;			/* size of data read by read()	*/

	/* if no block is created by pg_bulkload, no work needed. */
	if (blkbeg <= blkend)
		return;

	/*
	 * Allocate buffer page and blank pages with malloc so that the buffers
	 * will be well-aligned.
	 */
	page = palloc(BLCKSZ);
	zeropage = (Page) palloc(BLCKSZ);
	PageInit(zeropage, BLCKSZ, 0);

	/*
	 * get file name of first file name from ls.
	 *	   open the file.
	 *	   if size of the file is over than 1 file segment size(default 1GB),
	 *	   set	proper extension.
	 */
	segno = blkbeg / RELSEG_SIZE;
	GetSegmentPath(segpath,
#if PG_VERSION_NUM >= 160000
				   rLocator,
#else
				   rnode,
#endif
				   segno);

	/*
	 * TODO: consider to use truncate instead of zero-fill to end of file.
	 */

	fd = polar_open(segpath, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd == -1)
		elog(ERROR,
			 "could not open data file \"%s\": %m",
			 segpath);

	seekpos = polar_lseek(fd, (blkbeg % RELSEG_SIZE) * BLCKSZ, SEEK_SET);

	if (seekpos == -1)
		elog(ERROR,
			 "could not seek the target position in the data file \"%s\": %m",
			 segpath);

	blknum = blkbeg;

	/*
	 * pages created by pg_bulklod, overwrite them by blank pages.
	 */
	for (;;)
	{
		readlen = 0;
		ret = 0;

		/*
		 * to judge the page is created by pg_bulkload or not,
		 * read target blocks.
		 */
		do
		{
			ret = polar_read(fd, page + readlen, BLCKSZ - readlen);
			if (ret == -1)
			{
				if (errno == EAGAIN || errno == EINTR)
					continue;
				else
					elog(ERROR,
						 "could not read data file \"%s\": %m",
						 segpath);
			}
			else if (ret == 0)
			{
				/*
				 * case of partially writing, refill 0.
				 */
				memset(page + readlen, 0, BLCKSZ - readlen);
				ret = BLCKSZ - readlen;
			}
			readlen += ret;
		}
		while (readlen < BLCKSZ);


		/*
		 * if page is created by pg_bulkload, overwrite it by blank page.
		 */
		if (IsPageCreatedByLoader((Page) page))
		{
			seekpos = polar_lseek(fd, (blknum % RELSEG_SIZE) * BLCKSZ, SEEK_SET);

			if (polar_write(fd, zeropage, BLCKSZ) == -1)
				elog(ERROR,
					 "could not write correct empty page: %m");
		}

		blknum++;

		if (blknum >= blkend)
			break;

		/*
		 * if current block reach to the end of file, and need to process continuously,
		 * open next segment file.
		 */
		if (blknum % RELSEG_SIZE == 0)
		{
			if (polar_fsync(fd) != 0)
				elog(ERROR,
					 "could not sync data file \"%s\": %m",
					 segpath);

			if (polar_close(fd) == -1)
				elog(ERROR,
					 "could not close data file \"%s\": %m",
					 segpath);

			++segno;
			GetSegmentPath(segpath,
#if PG_VERSION_NUM >= 160000
						   rLocator,
#else
						   rnode,
#endif
						   segno);

			fd = polar_open(segpath, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
			if (fd == -1)
				elog(ERROR,
					 "could not open data file \"%s\": %m",
					 segpath);
		}
	}

	/*
	 * post process
	 */
	if (polar_fsync(fd) != 0)
		elog(ERROR,
			 "could not sync data file \"%s\": %m",
			 segpath);

	if (polar_close(fd) == -1)
		elog(ERROR,
			 "could not close data file \"%s\": %m",
			 segpath);

	pfree(page);
	pfree(zeropage);
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
			 * overwrite pages created by the loader by blank pages
			 */
			ClearLoadedPage(
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
