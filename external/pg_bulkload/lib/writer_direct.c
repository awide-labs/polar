/*
 * pg_bulkload: lib/writer_direct.c
 *
 *	  Copyright (c) 2007-2026, NTT, Inc.
 */

#include "pg_bulkload.h"

/* Fault injector support for crash recovery testing */
#ifdef FAULT_INJECTOR
#include "utils/faultinjector.h"
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/transam.h"
#if PG_VERSION_NUM >= 130000
#include "access/heaptoast.h"
#else
#include "access/tuptoaster.h"
#endif
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "catalog/storage_xlog.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/polar_fd.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "storage/bufpage.h"

#include "logger.h"
#include "pg_loadstatus.h"
#include "reader.h"
#include "writer.h"
#include "pg_btree.h"
#include "pg_profile.h"
#include "pg_strutil.h"
#include "pgut/pgut-be.h"

#if PG_VERSION_NUM >= 90300
#include "common/relpath.h"
#include "access/heapam_xlog.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"
#endif

#if PG_VERSION_NUM >= 90500
#include "access/xloginsert.h"
#endif

#if PG_VERSION_NUM >= 100000
#include "utils/regproc.h"
#endif

#if PG_VERSION_NUM >= 90400

#define log_newpage(rnode, forknum, blk, page) \
	log_newpage(rnode, forknum, blk, page, true)

#elif PG_VERSION_NUM < 80400

#define toast_insert_or_update(rel, newtup, oldtup, options) \
	toast_insert_or_update((rel), (newtup), (oldtup), true, true)

#define log_newpage(rnode, forknum, blk, page) \
	log_newpage((rnode), (blk), (page))

#endif

/**
 *  * pg_tli is removed in 9.3 and added pg_checksum instead
 *   */
#if PG_VERSION_NUM >= 90300
#define PageSetTLI(page, tli) \
	(((PageHeader) (page))->pd_checksum = (uint16) (0))
#endif

/**
 * @brief Heap loader using direct path
 */
typedef struct DirectWriter
{
	Writer			base;

	Spooler			spooler;

	LoadStatus		ls;
	int				lsf_fd;		/**< File descriptor of load status file */
	char			lsf_path[MAXPGPATH];	/**< Load status file path */

	TransactionId	xid;
	CommandId		cid;

	char		   *blocks;		/**< Local heap block buffer */
	int				curblk;		/**< Index of the current block buffer */
} DirectWriter;

/**
 * @brief Number of the block buffer
 */
#define BLOCK_BUF_NUM		1024

static void	DirectWriterInit(DirectWriter *self);
static void	DirectWriterInsert(DirectWriter *self, HeapTuple tuple);
static WriterResult	DirectWriterClose(DirectWriter *self, bool onError);
static bool	DirectWriterParam(DirectWriter *self, const char *keyword, char *value);
static void	DirectWriterDumpParams(DirectWriter *self);
static int	DirectWriterSendQuery(DirectWriter *self, PGconn *conn, char *queueName, char *logfile, bool verbose);

#define GetCurrentPage(self) \
			((Page) ((self)->blocks + BLCKSZ * (self)->curblk))
#define GetTargetPage(self, blk_offset) \
		((Page) ((self)->blocks + BLCKSZ * (blk_offset)))

/**
 * @brief Total number of blocks at the time
 */
#define LS_TOTAL_CNT(ls)	((ls)->ls.exist_cnt + (ls)->ls.create_cnt)

/* Signature of static functions */
static void	flush_pages(DirectWriter *loader);
static void	UpdateLSF(DirectWriter *loader, BlockNumber num);
static void UnlinkLSF(DirectWriter *loader);

/* ========================================================================
 * Implementation
 * ========================================================================*/

/**
 * @brief Create a new DirectWriter
 */
Writer *
CreateDirectWriter(void *opt)
{
	DirectWriter	   *self;

	self = palloc0(sizeof(DirectWriter));
	self->base.init = (WriterInitProc) DirectWriterInit;
	self->base.insert = (WriterInsertProc) DirectWriterInsert,
	self->base.close = (WriterCloseProc) DirectWriterClose,
	self->base.param = (WriterParamProc) DirectWriterParam;
	self->base.dumpParams = (WriterDumpParamsProc) DirectWriterDumpParams,
	self->base.sendQuery = (WriterSendQueryProc) DirectWriterSendQuery;
	self->base.max_dup_errors = -2;
	self->lsf_fd = -1;
	self->blocks = palloc_aligned(BLCKSZ * BLOCK_BUF_NUM, PG_IO_ALIGN_SIZE, 0);
	self->curblk = 0;

	return (Writer *) self;
}

/**
 * @brief Initialize a DirectWriter
 */
static void
DirectWriterInit(DirectWriter *self)
{
	LoadStatus		   *ls;
	char				lsf_dir[MAXPGPATH];

	/*
	 * Set defaults to unspecified parameters.
	 */
	if (self->base.max_dup_errors < -1)
		self->base.max_dup_errors = DEFAULT_MAX_DUP_ERRORS;
#if PG_VERSION_NUM >= 130000
	self->base.rel = table_open(self->base.relid, AccessExclusiveLock);
#else
	self->base.rel = heap_open(self->base.relid, AccessExclusiveLock);
#endif
	VerifyTarget(self->base.rel, self->base.max_dup_errors);

	self->base.desc = RelationGetDescr(self->base.rel);

	SpoolerOpen(&self->spooler, self->base.rel, false, self->base.on_duplicate,
				self->base.max_dup_errors, self->base.dup_badfile);
	self->base.context = GetPerTupleMemoryContext(self->spooler.estate);

	/* Verify pg_bulkload directory on shared storage */
	polar_make_file_path_level2(lsf_dir, BULKLOAD_LSF_DIR);
	ValidateLSFDirectory(lsf_dir);

	/* Initialize first block */
	PageInit(GetCurrentPage(self), BLCKSZ, 0);
	PageSetTLI(GetCurrentPage(self), ThisTimeLineID);

	/* Obtain transaction ID and command ID. */
	self->xid = GetCurrentTransactionId();
	self->cid = GetCurrentCommandId(true);

	/*
	 * Initialize load status information
	 */
	ls = &self->ls;
	ls->ls.relid = self->base.relid;
#if PG_VERSION_NUM >= 160000
	ls->ls.rLocator = self->base.rel->rd_locator;
#else
	ls->ls.rnode = self->base.rel->rd_node;
#endif
	ls->ls.exist_cnt = RelationGetNumberOfBlocks(self->base.rel);
	ls->ls.create_cnt = 0;

	/*
	 * Create a load status file and write the initial status for it.
	 * At the time, if we find any existing load status files, exit with
	 * error because recovery process haven't been executed after failing
	 * load to the same table.
	 */
	make_lsf_path(self->lsf_path, ls);
	self->lsf_fd = polar_open(self->lsf_path,
		O_CREAT | O_EXCL | O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (self->lsf_fd == -1)
		ereport(ERROR, (errcode_for_file_access(),
			errmsg("could not create loadstatus file \"%s\": %m", self->lsf_path)));

	if (polar_write(self->lsf_fd, ls, sizeof(LoadStatus)) != sizeof(LoadStatus) ||
		polar_fsync(self->lsf_fd) != 0)
	{
		UnlinkLSF(self);
		ereport(ERROR, (errcode_for_file_access(),
			errmsg("could not write loadstatus file \"%s\": %m", self->lsf_path)));
	}

	self->base.tchecker = CreateTupleChecker(self->base.desc);
	self->base.tchecker->checker = (CheckerTupleProc) CoercionCheckerTuple;
}

/**
 * @brief Create LoadStatus file and load heap tuples directly.
 * @return void
 */
static void
DirectWriterInsert(DirectWriter *self, HeapTuple tuple)
{
	Page			page;
	OffsetNumber	offnum;
	ItemId			itemId;
	Item			item;
	LoadStatus	   *ls = &self->ls;

	/* Compress the tuple data if needed. */
	if (tuple->t_len > TOAST_TUPLE_THRESHOLD)
#if PG_VERSION_NUM >= 130000
		tuple = heap_toast_insert_or_update(self->base.rel, tuple, NULL, 0);
#else
		tuple = toast_insert_or_update(self->base.rel, tuple, NULL, 0);
#endif
	BULKLOAD_PROFILE(&prof_writer_toast);

#if PG_VERSION_NUM < 120000
	/* Assign oids if needed. */
	if (self->base.rel->rd_rel->relhasoids)
	{
		Assert(!OidIsValid(HeapTupleGetOid(tuple)));
		HeapTupleSetOid(tuple, GetNewOid(self->base.rel));
	}
#endif

	/* Assume the tuple has been toasted already. */
	if (MAXALIGN(tuple->t_len) > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %lu, maximum size %lu",
						(unsigned long) tuple->t_len,
						(unsigned long) MaxHeapTupleSize)));

	/* Fill current page, or go to next page if the page is full. */
	page = GetCurrentPage(self);
	if (PageGetFreeSpace(page) < MAXALIGN(tuple->t_len) +
		RelationGetTargetPageFreeSpace(self->base.rel, HEAP_DEFAULT_FILLFACTOR))
	{

		
		if (self->curblk < BLOCK_BUF_NUM - 1)
			self->curblk++;
		else
		{
			flush_pages(self);
			self->curblk = 0;	/* recycle from first block */
		}

		page = GetCurrentPage(self);

		/* Initialize current block */
		PageInit(page, BLCKSZ, 0);
		PageSetTLI(page, ThisTimeLineID);
	}

	tuple->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tuple->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tuple->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tuple->t_data, self->xid);
	HeapTupleHeaderSetCmin(tuple->t_data, self->cid);
	HeapTupleHeaderSetXmax(tuple->t_data, 0);

	/* put the tuple on local page. */
	offnum = PageAddItem(page, (Item) tuple->t_data,
		tuple->t_len, InvalidOffsetNumber, false, true);

	ItemPointerSet(&(tuple->t_self), LS_TOTAL_CNT(ls) + self->curblk, offnum);
	itemId = PageGetItemId(page, offnum);
	item = PageGetItem(page, itemId);
	((HeapTupleHeader) item)->t_ctid = tuple->t_self;

	BULKLOAD_PROFILE(&prof_writer_table);
	SpoolerInsert(&self->spooler, tuple);
	BULKLOAD_PROFILE(&prof_writer_index);
}

/**
 * @brief Clean up load status information
 *
 * @param self [in/out] Load status information
 * @return void
 */
static WriterResult
DirectWriterClose(DirectWriter *self, bool onError)
{
	WriterResult	ret = { 0 };

	Assert(self != NULL);

	/* Flush unflushed block buffer. */
	if (!onError)
	{
		flush_pages(self);


		/*
		 * Fsync the relation.  Direct writer skips WAL, so fsync is the only
		 * guarantee that data survives a crash.
		 */
		if (self->ls.ls.create_cnt > 0)
			smgrimmedsync(RelationGetSmgr(self->base.rel), MAIN_FORKNUM);

		/*
		 * Emit a WAL record so the replica invalidates its RSC entry for this
		 * relation.  Without this the replica never learns about the new blocks
		 * added by the direct writer.  Skipped for temp and unlogged relations
		 * that don't stream WAL.
		 */
		if (self->ls.ls.create_cnt > 0 &&
			!RELATION_IS_LOCAL(self->base.rel) &&
			self->base.rel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED)
		{
			xl_smgr_bulk_extend xlrec;

			xlrec.rnode = self->base.rel->rd_node;
			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xlrec));
			XLogInsert(RM_SMGR_ID, XLOG_SMGR_BULK_EXTEND);
		}
	}

	UnlinkLSF(self);

	if (!onError)
	{
		SpoolerClose(&self->spooler);
		ret.num_dup_new = self->spooler.dup_new;
		ret.num_dup_old = self->spooler.dup_old;

		if (self->base.rel)
#if PG_VERSION_NUM >= 130000
			table_close(self->base.rel, AccessExclusiveLock);
#else
			heap_close(self->base.rel, AccessExclusiveLock);
#endif

		if (self->blocks)
			pfree(self->blocks);

		pfree(self);
	}

	return ret;
}

static bool
DirectWriterParam(DirectWriter *self, const char *keyword, char *value)
{
	if (CompareKeyword(keyword, "TABLE") ||
		CompareKeyword(keyword, "OUTPUT"))
	{
		ASSERT_ONCE(self->base.output == NULL);

		self->base.relid = RangeVarGetRelid(makeRangeVarFromNameList(
#if PG_VERSION_NUM >= 160000
						stringToQualifiedNameList(value, NULL)), NoLock, false);
#else
						stringToQualifiedNameList(value)), NoLock, false);
#endif
		self->base.output = get_relation_name(self->base.relid);
	}
	else if (CompareKeyword(keyword, "DUPLICATE_BADFILE"))
	{
		ASSERT_ONCE(self->base.dup_badfile == NULL);
		self->base.dup_badfile = pstrdup(value);
	}
	else if (CompareKeyword(keyword, "DUPLICATE_ERRORS"))
	{
		ASSERT_ONCE(self->base.max_dup_errors < -1);
		self->base.max_dup_errors = ParseInt64(value, -1);
		if (self->base.max_dup_errors == -1)
			self->base.max_dup_errors = INT64_MAX;
	}
	else if (CompareKeyword(keyword, "ON_DUPLICATE_KEEP"))
	{
		const ON_DUPLICATE values[] =
		{
			ON_DUPLICATE_KEEP_NEW,
			ON_DUPLICATE_KEEP_OLD
		};

		self->base.on_duplicate = values[choice(keyword, value, ON_DUPLICATE_NAMES, lengthof(values))];
	}
	else if (CompareKeyword(keyword, "TRUNCATE"))
	{
		self->base.truncate = ParseBoolean(value);
	}
	else
		return false;	/* unknown parameter */

	return true;
}

static void
DirectWriterDumpParams(DirectWriter *self)
{
	char		   *str;
	StringInfoData	buf;

	initStringInfo(&buf);

	appendStringInfoString(&buf, "WRITER = DIRECT\n");

	str = QuoteString(self->base.dup_badfile);
	appendStringInfo(&buf, "DUPLICATE_BADFILE = %s\n", str);
	pfree(str);

	if (self->base.max_dup_errors == INT64_MAX)
		appendStringInfo(&buf, "DUPLICATE_ERRORS = INFINITE\n");
	else
		appendStringInfo(&buf, "DUPLICATE_ERRORS = " int64_FMT "\n",
						 self->base.max_dup_errors);

	appendStringInfo(&buf, "ON_DUPLICATE_KEEP = %s\n",
					 ON_DUPLICATE_NAMES[self->base.on_duplicate]);

	appendStringInfo(&buf, "TRUNCATE = %s\n",
					 self->base.truncate ? "YES" : "NO");

	LoggerLog(INFO, buf.data, 0);
	pfree(buf.data);
}

static int
DirectWriterSendQuery(DirectWriter *self, PGconn *conn, char *queueName, char *logfile, bool verbose)
{
	const char *params[8];
	char		max_dup_errors[MAXINT8LEN + 1];

	if (self->base.max_dup_errors < -1)
		self->base.max_dup_errors = DEFAULT_MAX_DUP_ERRORS;

	snprintf(max_dup_errors, MAXINT8LEN, INT64_FORMAT,	
			 self->base.max_dup_errors);

	/* async query send */
	params[0] = queueName;
	params[1] = self->base.output;
	params[2] = ON_DUPLICATE_NAMES[self->base.on_duplicate];
	params[3] = max_dup_errors;
	params[4] = self->base.dup_badfile;
	params[5] = logfile;
	params[6] = verbose ? "true" : "no";
	params[7] = (self->base.truncate ? "true" : "no");

	return PQsendQueryParams(conn,
		"SELECT * FROM pgbulkload.pg_bulkload(ARRAY["
		"'TYPE=TUPLE',"
		"'INPUT=' || $1,"
		"'WRITER=DIRECT',"
		"'OUTPUT=' || $2,"
		"'ON_DUPLICATE_KEEP=' || $3,"
		"'DUPLICATE_ERRORS=' || $4,"
		"'DUPLICATE_BADFILE=' || $5,"
		"'LOGFILE=' || $6,"
		"'VERBOSE=' || $7,"
		"'TRUNCATE=' || $8])",
		8, NULL, params, NULL, NULL, 0);
}

/**
 * @brief Write block buffer contents.	Number of block buffer to be
 * written is specified by num argument.
 *
 * Flow:
 * <ol>
 *	 <li>Compute checksums for all buffered blocks.</li>
 *	 <li>Save the last block number in the load status file.</li>
 *	 <li>Write all blocks via polar_smgrbulkextend (handles segment boundaries).</li>
 * </ol>
 *
 * @param loader [in] Direct Writer.
 */
static void
flush_pages(DirectWriter *loader)
{
	int			num;
	LoadStatus *ls = &loader->ls;
	BlockNumber	blkno;
	SMgrRelation smgr;

	num = loader->curblk;
	if (!PageIsEmpty(GetCurrentPage(loader)))
		num += 1;

	if (num <= 0)
		return;		/* no work */

	/*
	 * Log the first page that pg_bulkload adds to WAL to ensure the current
	 * XID will be recorded in xlog.
	 *
	 * In recovery mode, PostgreSQL recognizes the current XID which was
	 * already assigned by reading through the xlog.
	 *
	 * As for pg_bulkload, if the first page WAL entry were not recorded,
	 * PostgreSQL would not remember the XID being used for this loading.
	 * This may cause an inconsistent database state after recovery.
	 *
	 * For example,
	 * 1. pg_bulkload is started in XID=1111.
	 * 2. PostgreSQL process crashes during the loading.
	 * 3. PostgreSQL drops all existing connections and begins crash recovery
	 *    with xlog. If pg_bulkload had not logged the first page, PostgreSQL
	 *    would (wrongly) fail to recognize that 1111 has been used.
	 * 4. After recovery, a new transaction would get 1111 as XID. If that
	 *    transaction commits eventually, the data insufficiently loaded by
	 *    pg_bulkload would be incorrectly visible because the loaded data
	 *    would have the same XID.
	 *
	 * In order to prevent that, we arrange that the first page added by
	 * pg_bulkload is logged to WAL.
	 */
#if PG_VERSION_NUM >= 90100
	if (ls->ls.create_cnt == 0 && !RELATION_IS_LOCAL(loader->base.rel)
			&& !(loader->base.rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED) )
	{
		XLogRecPtr	recptr;

		recptr = log_newpage(
#if PG_VERSION_NUM >= 160000
				&ls->ls.rLocator,
#else
				&ls->ls.rnode,
#endif
				MAIN_FORKNUM,
			ls->ls.exist_cnt, loader->blocks);
		XLogFlush(recptr);
	}
#else
	if (ls->ls.create_cnt == 0 && !RELATION_IS_LOCAL(loader->base.rel) )
	{
		XLogRecPtr	recptr;

		recptr = log_newpage(&ls->ls.rnode, MAIN_FORKNUM,
			ls->ls.exist_cnt, loader->blocks);
		XLogFlush(recptr);
	}
#endif

	blkno = LS_TOTAL_CNT(ls);

#if PG_VERSION_NUM >= 90300
	if (DataChecksumsEnabled())
	{
		int		j;

		for (j = 0; j < num; j++)
		{
			Page	contained_page = GetTargetPage(loader, j);
			PageSetChecksumInplace(contained_page, blkno + j);
		}
	}
#endif

	/* Write the last block number to the load status file. */
	UpdateLSF(loader, num);

	/*
	 * Write all blocks via the storage manager.  polar_smgrbulkextend
	 * handles relation segment boundaries internally.
	 */
	smgr = RelationGetSmgr(loader->base.rel);
	polar_smgrbulkextend(smgr, MAIN_FORKNUM, blkno, num, loader->blocks, true);

	/*
	 * Fault injection point for crash recovery testing.
	 * Use: SELECT inject_fault('pg_bulkload_during_write', 'panic');
	 * Fires only after more than one relation segment has been written,
	 * so the LSF records blocks beyond the first segment boundary.
	 */
#if defined(FAULT_INJECTOR)
	if (blkno >= BLOCK_BUF_NUM)
		SIMPLE_FAULT_INJECTOR("pg_bulkload_during_write");
#endif

	/*
	 * NOTICE: Be sure reset curblk to 0 and reinitialize recycled page
	 * if you will continue to use blocks.
	 */
}

/**
 * @brief Update load status file.
 * @param loader [in/out] Load status information
 * @param num [in] the number of blocks already written
 * @return void
 */
static void
UpdateLSF(DirectWriter *loader, BlockNumber num)
{
	int			ret;
	LoadStatus *ls = &loader->ls;

	ls->ls.create_cnt += num;

	polar_lseek(loader->lsf_fd, 0, SEEK_SET);
	ret = polar_write(loader->lsf_fd, ls, sizeof(LoadStatus));
	if (ret != sizeof(LoadStatus))
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("could not write to \"%s\": %m",
							   loader->lsf_path)));
	if (polar_fsync(loader->lsf_fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", loader->lsf_path)));
}

static void
UnlinkLSF(DirectWriter *loader)
{
	if (loader->lsf_fd != -1)
	{
		polar_close(loader->lsf_fd);
		loader->lsf_fd = -1;
		if (polar_unlink(loader->lsf_path) < 0 && errno != ENOENT)
			ereport(ERROR, (errcode_for_file_access(),
						errmsg("could not unlink load status file: %m")));
	}
}

/*
 * Check for LSF directory. If not exists, create it.
 */
void
ValidateLSFDirectory(const char *path)
{
	struct stat	stat_buf;

	if (polar_stat(path, &stat_buf) == 0)
	{
		/* Check for weird cases where it exists but isn't a directory */
		if (!S_ISDIR(stat_buf.st_mode))
			ereport(ERROR,
			(errmsg("pg_bulkload: required LSF directory \"%s\" does not exist",
							path)));
	}
	else
	{
		ereport(LOG,
				(errmsg("pg_bulkload: creating missing LSF directory \"%s\"", path)));
		if (polar_mkdir(path, 0700) < 0)
			ereport(ERROR,
					(errmsg("could not create missing directory \"%s\": %m",
							path)));
	}
}
