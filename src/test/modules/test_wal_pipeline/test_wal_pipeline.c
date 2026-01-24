#include "postgres.h"
#include "pgstat.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "access/xlog.h"
#include "catalog/pg_control.h"

PG_MODULE_MAGIC;

pthread_t	advance_handle;
pthread_t	write_handle;
pthread_t	flush_handle;
pthread_t	notify_handle;
int			ident = 0;

PG_FUNCTION_INFO_V1(test_wal_pipeline);

static void *
test_worker_for_advance(void *arg)
{
	pg_usleep(1000000);			/* wait 1s */
	Assert(MyProc->wait_event_info == WAIT_EVENT_WAL_PIPELINE_WAIT_RECENT_WRITTEN_SPACE);
	polar_wal_pipeline_advance(0);
	pg_usleep(1000000);			/* wait 1s */
	Assert(MyProc->wait_event_info == 0);

	pthread_exit(0);
}

/*
static void *
test_worker_for_write(void *arg)
{
	pg_usleep(1000000);
	polar_wal_pipeline_write(0);
	polar_wal_pipeline_flush(0);
	polar_wal_pipeline_notify(0);

	pthread_exit(0);
}
*/

static void
test_wal_pipeline_advance(void)
{
	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	elog(INFO, "case 1 test recovery mode");
	polar_wal_pipeline_set_local_recovery_mode(true);
	Assert(!polar_wal_pipeline_advance(0));
	polar_wal_pipeline_set_local_recovery_mode(false);
	elog(INFO, "case 1 test recovery mode ok");

	polar_wal_pipeline_set_ready_write_lsn(40);

	elog(INFO, "case 2 test add link");
	polar_wal_pipeline_recent_written_add_link(40, 60);
	polar_wal_pipeline_advance(0);
	Assert(polar_wal_pipeline_get_ready_write_lsn() == 60);
	elog(INFO, "case 2 test add link ok");

	elog(INFO, "case 3 test no space");
	polar_wal_pipeline_recent_written_add_link(60, 1084);
	Assert(MyProc->wait_event_info == 0);
	pthread_create(&advance_handle, NULL, test_worker_for_advance, &ident);
	/* polar_wal_pipeline_recent_written_array_size is 1024 */
	polar_wal_pipeline_recent_written_add_link(1084, 1094);
	pthread_join(advance_handle, NULL);
	polar_wal_pipeline_advance(0);
	Assert(polar_wal_pipeline_get_ready_write_lsn() == 1094);
	elog(INFO, "case 3 test no space ok");
}

static void
test_wal_pipeline_write(void)
{
/* 	XLogRecPtr lsn; */
/* 	char data[1048576] = {0}; */

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	elog(INFO, "case 1 test recovery mode");
	polar_wal_pipeline_set_local_recovery_mode(true);
	Assert(!polar_wal_pipeline_write(0));
	polar_wal_pipeline_set_local_recovery_mode(false);
	elog(INFO, "case 1 test recovery mode ok");

	elog(INFO, "case 2 test write nothing");
	polar_wal_pipeline_set_ready_write_lsn(polar_wal_pipeline_get_write_lsn() - 1);
	Assert(!polar_wal_pipeline_write(0));
	elog(INFO, "case 2 test write nothing ok");

#if 0
	elog(INFO, "case 3 test write only");
	XLogBeginInsert();
	XLogRegisterData(data, sizeof(data));
	lsn = XLogInsert(RM_XLOG_ID, XLOG_NOOP);
	polar_wal_pipeline_set_ready_write_lsn(lsn);
	polar_wal_pipeline_write(0);
	Assert(polar_wal_pipeline_get_write_lsn() == lsn);
	elog(INFO, "case 3 test write only ok");

	elog(INFO, "case 4 test flush only");
	pthread_create(&write_handle, NULL, test_worker_for_write, &ident);
	polar_wal_pipeline_commit_wait(lsn);
	pthread_join(write_handle, NULL);
	Assert(polar_wal_pipeline_get_flush_lsn() == lsn);
	elog(INFO, "case 4 test flush only ok");
#endif
}

static void
test_wal_pipeline_flush(void)
{
	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	elog(INFO, "case 1 test recovery mode");
	polar_wal_pipeline_set_local_recovery_mode(true);
	Assert(!polar_wal_pipeline_flush(0));
	polar_wal_pipeline_set_local_recovery_mode(false);
	elog(INFO, "case 1 test recovery mode ok");
}

static void
test_wal_pipeline_notify(void)
{
	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	elog(INFO, "case 1 test recovery mode");
	polar_wal_pipeline_set_local_recovery_mode(true);
/* 	Assert(!polar_wal_pipeline_notify(0)); */
	polar_wal_pipeline_set_local_recovery_mode(false);
	elog(INFO, "case 1 test recovery mode ok");
}

/*
 * SQL-callable entry point to perform all tests.
 */
Datum
test_wal_pipeline(PG_FUNCTION_ARGS)
{
	if (polar_wal_pipeline_enable)
		PG_RETURN_VOID();

	test_wal_pipeline_advance();

	test_wal_pipeline_write();

	test_wal_pipeline_flush();

	test_wal_pipeline_notify();

	PG_RETURN_VOID();
}
