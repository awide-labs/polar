/* external/polar_monitor/polar_monitor--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_monitor" to load this file. \quit

-- Register the function.
CREATE FUNCTION polar_consistent_lsn()
RETURNS pg_lsn
AS 'MODULE_PATHNAME', 'polar_consistent_lsn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_oldest_apply_lsn()
RETURNS pg_lsn
AS 'MODULE_PATHNAME', 'polar_oldest_apply_lsn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_oldest_lock_lsn()
RETURNS pg_lsn
AS 'MODULE_PATHNAME', 'polar_oldest_lock_lsn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_replica_min_used_lsn()
RETURNS pg_lsn
AS 'MODULE_PATHNAME', 'polar_replica_min_used_lsn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_min_used_lsn()
RETURNS pg_lsn
AS 'MODULE_PATHNAME', 'polar_min_used_lsn'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_get_logindex_mem_tbl_size()
RETURNS bigint
AS 'MODULE_PATHNAME', 'polar_get_logindex_mem_tbl_size'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_used_logindex_mem_tbl_size()
RETURNS bigint
AS 'MODULE_PATHNAME', 'polar_used_logindex_mem_tbl_size'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_replica_use_xlog_queue()
RETURNS bool
AS 'MODULE_PATHNAME', 'polar_replica_use_xlog_queue'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_replica_bg_replay_lsn()
RETURNS pg_lsn
AS 'MODULE_PATHNAME', 'polar_replica_bg_replay_lsn'
LANGUAGE C PARALLEL SAFE;

-- Register the normal buffer function.
CREATE FUNCTION polar_get_normal_buffercache_pages()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'polar_get_normal_buffercache_pages'
LANGUAGE C PARALLEL SAFE;

-- Create a view for normal buffer convenient access.
CREATE VIEW polar_normal_buffercache AS
        SELECT P.* FROM polar_get_normal_buffercache_pages() AS P
            (bufferid integer, relfilenode oid, reltablespace oid, reldatabase oid,
             relforknumber int2, relblocknumber int8, isdirty bool, usagecount int2,
             oldest_lsn pg_lsn, newest_lsn pg_lsn, flushnext int4, flushprev int4,
             incopybuf bool,first_touched_after_copy bool, pinning_backends int4,
             recently_modified_count int2, oldest_lsn_is_fake bool);

-- Don't want these to be available to public.
REVOKE ALL ON FUNCTION polar_get_normal_buffercache_pages() FROM PUBLIC;
REVOKE ALL ON polar_normal_buffercache FROM PUBLIC;

-- Register the copy buffer function.
CREATE FUNCTION polar_get_copy_buffercache_pages()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'polar_get_copy_buffercache_pages'
LANGUAGE C PARALLEL SAFE;

-- Create a view for copy buffer convenient access.
CREATE VIEW polar_copy_buffercache AS
        SELECT P.* FROM polar_get_copy_buffercache_pages() AS P
        (bufferid integer, relfilenode oid, reltablespace oid, reldatabase oid,
         relforknumber int2, relblocknumber int8, freenext int4, passcount int4,
         state int2, oldest_lsn pg_lsn, newest_lsn pg_lsn, is_flushed bool);

-- Don't want these to be available to public.
REVOKE ALL ON FUNCTION polar_get_copy_buffercache_pages() FROM PUBLIC;
REVOKE ALL ON polar_copy_buffercache FROM PUBLIC;

CREATE FUNCTION polar_flushlist(OUT size int8,
                                OUT put int8,
                                OUT remove int8,
                                OUT find int8,
                                OUT batchread int8,
                                OUT cbuf int8,
                                OUT vm_put int8,
                                OUT vm_remove int8)
RETURNS record
AS 'MODULE_PATHNAME', 'polar_flushlist'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_cbuf(OUT flush int8,
                           OUT copy int8,
                           OUT unavailable int8,
                           OUT full int8,
                           OUT release int8)
RETURNS record
AS 'MODULE_PATHNAME', 'polar_cbuf'
LANGUAGE C PARALLEL SAFE;

/* Create backend flush buffer count func */
CREATE FUNCTION polar_backend_flush()
RETURNS int8
AS 'MODULE_PATHNAME', 'polar_backend_flush'
LANGUAGE C PARALLEL SAFE;

/* Create lru flush info func */
CREATE FUNCTION polar_lru_flush_info(OUT lru_complete_passes int4,
                                     OUT lru_buffer_id int4,
                                     OUT strategy_passes int4,
                                     OUT strategy_buf_id int4)
RETURNS RECORD
AS 'MODULE_PATHNAME', 'polar_lru_flush_info'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_node_type()
RETURNS text
AS 'MODULE_PATHNAME', 'polar_node_type'
LANGUAGE C PARALLEL SAFE;

-- Create customized pfsadm du func
CREATE FUNCTION pfs_du_with_depth(INT, TEXT)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pfs_du_with_depth'
LANGUAGE C PARALLEL SAFE;

-- Create customized pfsadm info func
CREATE FUNCTION pfs_info()
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pfs_info'
LANGUAGE C PARALLEL SAFE;

-- Create customized polar process info func
CREATE FUNCTION polar_stat_process(
            OUT pid oid,
            OUT wait_object int4,
            OUT wait_time float8,
            OUT cpu_user int8,
            OUT cpu_sys int8,
            OUT rss  int8,
            OUT shared_read_ps int8,
            OUT shared_write_ps int8,
            OUT shared_read_throughput int8,
            OUT shared_write_throughput int8,
            OUT shared_read_latency_ms float8,
            OUT shared_write_latency_ms float8,
            OUT local_read_ps int8,
            OUT local_write_ps int8,
            OUT local_read_throughput int8,
            OUT local_write_throughput int8,
            OUT local_read_latency_ms float8,
            OUT local_write_latency_ms float8,
            OUT wait_type text,
            OUT queryid int8
)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'polar_stat_process'
LANGUAGE C PARALLEL SAFE;


CREATE VIEW polar_stat_activity AS
    SELECT b.*, b.query_id AS queryid,
        (CASE WHEN a.wait_object IS NULL and a.wait_time IS NOT NULL THEN CAST(pg_blocking_pids(b.pid) as text) ELSE CAST(a.wait_object as text) END)::text AS wait_object,
        wait_type,wait_time as "wait_time_ms", cpu_user, cpu_sys, rss,
        shared_read_ps, shared_write_ps, shared_read_throughput, shared_write_throughput, shared_read_latency_ms, shared_write_latency_ms,
        local_read_ps, local_write_ps, local_read_throughput, local_write_throughput, local_read_latency_ms, local_write_latency_ms
    FROM pg_stat_activity AS b left join polar_stat_process() AS a on b.pid = a.pid;

-- Create customized polar IO info func
CREATE FUNCTION polar_stat_io_info(
            OUT pid oid,
            OUT filetype text,
            OUT fileloc text,
            OUT open_count int8,
            OUT open_latency_us float8,
            OUT close_count int8,
            OUT read_count int8,
            OUT write_count int8,
            OUT read_throughput int8,
            OUT write_throughput int8,
            OUT read_latency_us float8,
            OUT write_latency_us float8,
            OUT seek_count int8,
            OUT seek_latency_us float8,
            OUT creat_count int8,
            OUT creat_latency_us float8,
            OUT fsync_count int8,
            OUT fsync_latency_us float8,
            OUT falloc_count int8,
            OUT falloc_latency_us float8
)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'polar_stat_io_info'
LANGUAGE C PARALLEL SAFE;

CREATE VIEW polar_stat_io_info AS
    SELECT filetype, fileloc, sum(open_count) as open_count, sum(open_latency_us) as open_latency_us,
        sum(close_count) as close_count, sum(read_count) as read_count, sum(write_count) as write_count, sum(read_throughput)  as read_throughput,
        sum(write_throughput) as write_throughput,
        sum(read_latency_us) as read_latency_us, sum(write_latency_us) as write_latency_us, sum(seek_count) as seek_count, sum(seek_latency_us) as seek_latency_us,
        sum(creat_count) as creat_count, sum(creat_latency_us) as creat_latency_us, sum(fsync_count) as fsync_count,
        sum(fsync_latency_us) as fsync_latency_us, sum(falloc_count) as falloc_count, sum(falloc_latency_us) as falloc_latency_us
    FROM polar_stat_io_info() GROUP BY filetype, fileloc;

-- Create customized io latency info func
CREATE FUNCTION polar_io_latency_info(
            OUT pid oid,
            OUT IOLoc text,
            OUT IOKind text,
            OUT Num_LessThan200us int8,
            OUT Num_LessThan400us int8,
            OUT Num_LessThan600us int8,
            OUT Num_LessThan800us int8,
            OUT Num_LessThan1ms int8,
            OUT Num_LessThan10ms int8,
            OUT Num_LessThan100ms int8,
            OUT Num_MoreThan100ms int8
)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'polar_io_latency_info'
LANGUAGE C PARALLEL SAFE;

CREATE VIEW polar_stat_io_latency AS
    SELECT IOLoc, IOKind, sum(Num_LessThan200us) Num_LessThan200us, sum(Num_LessThan400us) Num_LessThan400us,
        sum(Num_LessThan600us) Num_LessThan600us, sum(Num_LessThan800us) Num_LessThan800us,
        sum(Num_LessThan1ms) Num_LessThan1ms, sum(Num_LessThan10ms) Num_LessThan10ms,
        sum(Num_LessThan100ms) Num_LessThan100ms, sum(Num_MoreThan100ms) Num_MoreThan100ms
    FROM polar_io_latency_info() GROUP BY IOKind, IOLoc;

/*
 * POLAR:
 * This is an naive implementation for getting delta value or watching delta
 * change on accumulated values.
 *
 * NOTE: we'd better not use this function online especially as a routine.
 *
 * What you need to do is:
 * 1. Create a view with dimension columns and value columns.
 *    - Dimension columns name must starts with 'd_', we use these columns as
 *      join condition.
 *    - Value columns name must starts with 'v_', we calculate delta for thes
 *      columns.
 *    - Other columns we just get newest value in view;
 * 2. Then use "select * from polar_delta(NULL::view_name)" to get delta value
 *    of view, or use "\watch 1 select * from polar_delta(NULL::view_name)".
 *
 * Follow up I give an simple example on watching cpu/mem/iops for every backends.
 */
CREATE OR REPLACE FUNCTION polar_delta(IN _tbl_type anyelement)
  RETURNS SETOF anyelement AS
$func$
DECLARE
    r record;
    pid int;
    sql text;
    firstselect bool;
    firstjoin bool;
    fieldstr text;
    join_condition text;
    old_tmp_table text;
    new_tmp_table text;
    real_tmp_table text;
BEGIN
    sql := ' ';
    join_condition := ' ';
    fieldstr := ' ';
    firstselect := true;
    firstjoin := true;
    pid := (SELECT PG_BACKEND_PID());
    old_tmp_table := pg_typeof(_tbl_type) || '_' || pid || '_old';
    new_tmp_table := pg_typeof(_tbl_type) || '_' || pid || '_new';
    real_tmp_table := pg_typeof(_tbl_type) || '_' || pid || '_tmp';

    SET LOCAL client_min_messages=ERROR;
    /* 1. try to create two tmp table */
    EXECUTE FORMAT('CREATE TEMP TABLE IF NOT EXISTS %s AS SELECT * FROM %s',
                    old_tmp_table, pg_typeof(_tbl_type));
    EXECUTE FORMAT('CREATE TEMP TABLE IF NOT EXISTS %s AS SELECT * FROM %s',
                    new_tmp_table, pg_typeof(_tbl_type));

    /* 2. exchange two tmp talbe for keeping old data in old_tmp_table  */
    EXECUTE FORMAT('ALTER TABLE %s RENAME TO %s', old_tmp_table, real_tmp_table);
    EXECUTE FORMAT('ALTER TABLE %s RENAME TO %s', new_tmp_table, old_tmp_table);
    EXECUTE FORMAT('ALTER TABLE %s RENAME TO %s', real_tmp_table, new_tmp_table);

    /* 3. truncate and insert data into new tmp table. */
    EXECUTE FORMAT('TRUNCATE TABLE %s', new_tmp_table);
    EXECUTE FORMAT('INSERT INTO %s SELECT * FROM %s', new_tmp_table, pg_typeof(_tbl_type));

    /* 4. Then we join old tmp table and new tmp table on dimension column to get delta values */
    FOR r IN SELECT attname FROM pg_class, pg_attribute WHERE pg_class.reltype=pg_typeof(_tbl_type) AND pg_class.oid=pg_attribute.attrelid ORDER BY attnum
    LOOP
        IF NOT firstselect THEN
            fieldstr := fieldstr || ', ';
        ELSE
            firstselect := false;
        END IF;

        IF starts_with(r.attname, 'd_') THEN
            IF NOT firstjoin THEN
                join_condition := join_condition || ' AND ';
            ELSE
                firstjoin := false;
            END IF;

            join_condition := join_condition || FORMAT(' old.%s = new.%s ', r.attname, r.attname);
            fieldstr := fieldstr || FORMAT(' new.%s ', r.attname);
        ELSE
            IF starts_with(r.attname, 'v_') THEN
                fieldstr := fieldstr || FORMAT(' new.%s - old.%s ', r.attname, r.attname);
            ELSE
                fieldstr := fieldstr || FORMAT(' new.%s', r.attname);
            END IF;
        END IF;
    END LOOP;

    sql := FORMAT('SELECT %s FROM %s new JOIN %s old ON %s',
                fieldstr, new_tmp_table, old_tmp_table, join_condition);

    RETURN QUERY EXECUTE sql;
END
$func$ LANGUAGE plpgsql;

/*
 * POLAR: with clause would not register meta in pg_class and pg_attribute
 *   so we can only create reduncdent view here now.
 */
CREATE VIEW polar_stat_activity_rt_mid AS
    SELECT pid AS d_pid
        , backend_type AS d_backend_type
        , cpu_user AS v_cpu_user
        , cpu_sys AS v_cpu_sys
        , rss AS rss
        , local_read_ps AS v_local_read_iops
        , local_write_ps AS v_local_write_iops
        , local_read_throughput AS v_local_read_iothroughput
        , local_write_throughput AS v_local_write_iothroughput
        , local_read_latency_ms AS v_local_read_latency
        , local_write_latency_ms AS v_local_write_latency
    FROM
        polar_stat_activity;

/*
 * POLAR: watch each backend cpu, rss, iops and so on.
 */
CREATE VIEW polar_stat_activity_rt AS
    SELECT d_pid AS pid
        , d_backend_type AS backend_type
        , v_cpu_user AS cpu_user
        , v_cpu_sys AS cpu_sys
        , ROUND(rss * 4096 / 1024 / 1024.0, 2) AS rss
        , v_local_read_iops AS local_read_iops
        , v_local_write_iops AS local_write_iops
        , ROUND(v_local_read_iothroughput / 1024 / 1024, 2) AS local_read_iothroughput
        , ROUND(v_local_write_iothroughput / 1024 / 1024, 2) AS local_write_iothroughput
        , v_local_read_latency AS local_read_latency
        , v_local_write_latency AS local_write_latency
    FROM polar_delta(NULL::polar_stat_activity_rt_mid);


CREATE FUNCTION polar_set_available(available bool)
RETURNS VOID
AS 'MODULE_PATHNAME', 'polar_set_available'
LANGUAGE C PARALLEL UNSAFE;
CREATE FUNCTION polar_is_available()
RETURNS BOOL
AS 'MODULE_PATHNAME', 'polar_is_available'
LANGUAGE C PARALLEL UNSAFE;
REVOKE ALL ON FUNCTION polar_set_available FROM PUBLIC;
REVOKE ALL ON FUNCTION polar_is_available FROM PUBLIC;

-- proxy monitor
CREATE FUNCTION polar_stat_get_pid(pid int default NULL)
RETURNS INT
AS 'MODULE_PATHNAME', 'polar_stat_get_pid'
LANGUAGE C PARALLEL SAFE;
CREATE FUNCTION polar_stat_get_sid(pid int default NULL)
RETURNS INT
AS 'MODULE_PATHNAME', 'polar_stat_get_sid'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_stat_proxy_info()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'polar_stat_proxy_info'
LANGUAGE C PARALLEL SAFE;
CREATE FUNCTION polar_stat_reset_proxy_info()
RETURNS void
AS 'MODULE_PATHNAME', 'polar_stat_reset_proxy_info'
LANGUAGE C PARALLEL SAFE;

CREATE VIEW polar_stat_proxy_info AS
	SELECT * FROM polar_stat_proxy_info() AS P (reason text, count int8);
CREATE VIEW polar_stat_proxy_info_rt_mid AS
	SELECT * FROM polar_stat_proxy_info() AS P
		(d_reason text, v_count int8);
CREATE VIEW polar_stat_proxy_info_rt AS
	SELECT * FROM polar_delta(NULL::polar_stat_proxy_info_rt_mid);
CREATE OR REPLACE FUNCTION polar_xlog_queue_stat_detail(
					OUT push_cnt int8,
                    OUT pop_cnt int8,
                    OUT free_up_cnt int8,
					OUT send_phys_io_cnt int8,
                    OUT total_written int8,
                    OUT total_read int8,
					OUT evict_ref_cnt int8,
					OUT queue_pwrite int8,
					OUT queue_pread int8,
					OUT queue_visit int8)
RETURNS record
AS 'MODULE_PATHNAME', 'polar_xlog_queue_stat_detail'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_get_xlog_queue_ref_info_func(
	OUT ref_name text,
	OUT ref_pread int8,
	OUT ref_visit int8,
	OUT is_strong int8,
	OUT ref_num int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_get_xlog_queue_ref_info_func'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE VIEW polar_get_xlog_queue_ref_info AS
    SELECT ref_name, ref_pread, ref_visit, is_strong, ref_num
    from polar_get_xlog_queue_ref_info_func() WHERE ref_name IS NOT NULL;

-- POLAR RSC

CREATE FUNCTION polar_rsc_stat_counters(
    OUT nblocks_pointer_hit int8,
    OUT nblocks_mapping_hit int8,
    OUT nblocks_mapping_miss int8,
    OUT mapping_update_hit int8,
    OUT mapping_update_evict int8,
    OUT mapping_update_invalidate int8
)
RETURNS RECORD
AS 'MODULE_PATHNAME', 'polar_monitor_stat_rsc_counters'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_rsc_stat_reset()
RETURNS VOID
AS 'MODULE_PATHNAME', 'polar_monitor_stat_rsc_counters_reset'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_rsc_check_consistency(
    IN relation regclass,
    IN forkname TEXT DEFAULT 'main'
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'polar_monitor_stat_rsc_check_consistency'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_stat_get_rsc_mem(
    IN BOOLEAN DEFAULT true,
    OUT idx int4,
    OUT spc_node OID,
    OUT db_node OID,
    OUT rel_node OID,
    OUT in_cache BOOLEAN,
    OUT entry_locked BOOLEAN,
    OUT entry_valid BOOLEAN,
    OUT entry_dirty BOOLEAN,
    OUT generation int8,
    OUT usecount int8,
    OUT main_cache int4,
    OUT fsm_cache int4,
    OUT vm_cache int4,
    OUT main_real int4,
    OUT fsm_real int4,
    OUT vm_real int4
)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'polar_monitor_stat_rsc_entries'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_stat_rsc_pool(
    OUT total_num int4,
    OUT locked_num int4,
    OUT valid_num int4,
    OUT dirty_num int4
)
RETURNS RECORD
LANGUAGE SQL
AS
    'SELECT
        COUNT(*) AS total_num,
        SUM(entry_locked::int) AS locked_num,
        SUM(entry_valid::int) AS valid_num,
        SUM(entry_dirty::int) AS dirty_num
    FROM polar_stat_get_rsc_mem(false)';

CREATE FUNCTION polar_stat_get_rsc_mem_flags(
    OUT sr_offset int4,
    OUT is_locked BOOLEAN,
    OUT is_valid BOOLEAN,
    OUT is_dirty BOOLEAN
)
RETURNS SETOF RECORD
LANGUAGE SQL
AS
    'SELECT
        idx AS sr_offset,
        entry_locked AS is_locked,
        entry_valid AS is_valid,
        entry_dirty AS is_dirty
    FROM polar_stat_get_rsc_mem(false)';

CREATE FUNCTION polar_rsc_clear_current_db()
RETURNS VOID
AS 'MODULE_PATHNAME', 'polar_monitor_stat_rsc_clear_current_db'
LANGUAGE C;

-- POLAR RSC end

CREATE FUNCTION polar_async_replay_lock()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'polar_async_replay_lock'
LANGUAGE C PARALLEL SAFE;

CREATE VIEW polar_async_replay_lock AS
    SELECT P.* FROM polar_async_replay_lock() AS P
        (xid xid, dbOid oid, relOid oid, lsn pg_lsn,
        rtime timestamptz, state text, running bool);

REVOKE ALL ON FUNCTION polar_async_replay_lock() FROM PUBLIC;

-- Fullpage Snapshot
CREATE FUNCTION polar_used_logindex_fullpage_snapshot_mem_tbl_size()
RETURNS bigint
AS 'MODULE_PATHNAME', 'polar_used_logindex_fullpage_snapshot_mem_tbl_size'
LANGUAGE C PARALLEL SAFE;
-- Fullpage Snapshot end

-- Create customized xlog queue free ratio func
CREATE OR REPLACE FUNCTION xlog_queue_stat_info(OUT xlog_queue_total_size int8,
                    OUT xlog_queue_free_size int8,
                    OUT xlog_queue_free_ratio float8)
RETURNS record
AS 'MODULE_PATHNAME', 'xlog_queue_stat_info'
LANGUAGE C PARALLEL SAFE;

-- bulk extend stats
-- bulk extend times
CREATE FUNCTION polar_pg_stat_get_bulk_extend_times(
    IN oid,
    OUT int8
)
AS 'MODULE_PATHNAME', 'polar_pg_stat_get_bulk_extend_times'
LANGUAGE C PARALLEL SAFE;

-- bulk extend counts
CREATE FUNCTION polar_pg_stat_get_bulk_extend_blocks(
    IN oid,
    OUT int8
)
AS 'MODULE_PATHNAME', 'polar_pg_stat_get_bulk_extend_blocks'
LANGUAGE C PARALLEL SAFE;

CREATE VIEW polar_pg_stat_bulk_extend_all_tables AS
    SELECT
        C.oid AS relid,
        N.nspname AS schemaname,
        C.relname AS relname,
        polar_pg_stat_get_bulk_extend_times(C.oid) AS heap_bulk_extend_times,
        polar_pg_stat_get_bulk_extend_blocks(C.oid) AS heap_bulk_extend_blocks,

        sum(polar_pg_stat_get_bulk_extend_times(I.indexrelid))::bigint AS idx_bulk_extend_times,
        sum(polar_pg_stat_get_bulk_extend_blocks(I.indexrelid))::bigint AS idx_bulk_extend_blocks,

        polar_pg_stat_get_bulk_extend_times(T.oid) AS toast_bulk_extend_times,
        polar_pg_stat_get_bulk_extend_blocks(T.oid) AS toast_bulk_extend_blocks,

        sum(polar_pg_stat_get_bulk_extend_times(X.indexrelid))::bigint AS tidx_bulk_extend_times,
        sum(polar_pg_stat_get_bulk_extend_blocks(X.indexrelid))::bigint AS tidx_bulk_extend_blocks

    FROM pg_class C LEFT JOIN
            pg_index I ON C.oid = I.indrelid LEFT JOIN
            pg_class T ON C.reltoastrelid = T.oid LEFT JOIN
            pg_index X ON T.oid = X.indrelid
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.relkind IN ('r', 't', 'm')
    GROUP BY C.oid, N.nspname, C.relname, T.oid, X.indrelid;
/* bulk read stats */
/* per table (or index) */
CREATE FUNCTION polar_pg_stat_get_bulk_read_calls(
            IN  oid,
            OUT int8
)
AS 'MODULE_PATHNAME', 'polar_pg_stat_get_bulk_read_calls'
LANGUAGE C PARALLEL SAFE;
REVOKE ALL ON FUNCTION polar_pg_stat_get_bulk_read_calls(IN oid, OUT int8) FROM PUBLIC;

CREATE FUNCTION polar_pg_stat_get_bulk_read_calls_IO(
            IN  oid,
            OUT int8
)
AS 'MODULE_PATHNAME', 'polar_pg_stat_get_bulk_read_calls_IO'
LANGUAGE C PARALLEL SAFE;
REVOKE ALL ON FUNCTION polar_pg_stat_get_bulk_read_calls_IO(IN oid, OUT int8) FROM PUBLIC;

CREATE FUNCTION polar_pg_stat_get_bulk_read_blocks_IO(
            IN  oid,
            OUT int8
)
AS 'MODULE_PATHNAME', 'polar_pg_stat_get_bulk_read_blocks_IO'
LANGUAGE C PARALLEL SAFE;
REVOKE ALL ON FUNCTION polar_pg_stat_get_bulk_read_blocks_IO(IN oid, OUT int8) FROM PUBLIC;

CREATE VIEW polar_pg_statio_all_tables AS
    SELECT
            C.oid AS relid,
            N.nspname AS schemaname,
            C.relname AS relname,
			polar_pg_stat_get_bulk_read_calls(C.oid) AS heap_bulk_read_calls,
			polar_pg_stat_get_bulk_read_calls_IO(C.oid) AS heap_bulk_read_calls_IO,
			polar_pg_stat_get_bulk_read_blocks_IO(C.oid) AS heap_bulk_read_blks_IO,

            sum(polar_pg_stat_get_bulk_read_calls(I.indexrelid))::bigint AS idx_bulk_read_calls,
            sum(polar_pg_stat_get_bulk_read_calls_IO(I.indexrelid))::bigint AS idx_bulk_read_calls_IO,
            sum(polar_pg_stat_get_bulk_read_blocks_IO(I.indexrelid))::bigint AS idx_bulk_read_blks_IO,

			polar_pg_stat_get_bulk_read_calls(T.oid) AS toast_bulk_read_calls,
			polar_pg_stat_get_bulk_read_calls_IO(T.oid) AS toast_bulk_read_calls_IO,
			polar_pg_stat_get_bulk_read_blocks_IO(T.oid) AS toast_bulk_read_blks_IO,

            sum(polar_pg_stat_get_bulk_read_calls(X.indexrelid))::bigint AS tidx_bulk_read_calls,
			sum(polar_pg_stat_get_bulk_read_calls_IO(X.indexrelid))::bigint AS tidx_bulk_read_calls_IO,
			sum(polar_pg_stat_get_bulk_read_blocks_IO(X.indexrelid))::bigint AS tidx_bulk_read_blocks_IO

    FROM pg_class C LEFT JOIN
            pg_index I ON C.oid = I.indrelid LEFT JOIN
            pg_class T ON C.reltoastrelid = T.oid LEFT JOIN
            pg_index X ON T.oid = X.indrelid
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace)
    WHERE C.relkind IN ('r', 't', 'm')
    GROUP BY C.oid, N.nspname, C.relname, T.oid, X.indrelid;

-- REVOKE bulk extend stats
REVOKE ALL ON FUNCTION polar_pg_stat_get_bulk_extend_times(IN oid, OUT int8) FROM PUBLIC;
REVOKE ALL ON FUNCTION polar_pg_stat_get_bulk_extend_blocks(IN oid, OUT int8) FROM PUBLIC;

CREATE VIEW polar_pg_statio_user_tables AS
    SELECT * FROM polar_pg_statio_all_tables
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

/* end: bulk read stats */
CREATE OR REPLACE FUNCTION polar_xlog_buffer_stat_info(
					OUT hit_count int8,
                    OUT io_count int8,
                    OUT others_append_count int8,
                    OUT startup_append_count int8)
RETURNS record
AS 'MODULE_PATHNAME', 'polar_xlog_buffer_stat_info'
LANGUAGE C PARALLEL SAFE;

REVOKE ALL ON FUNCTION polar_xlog_buffer_stat_info(OUT hit_count int8,
    OUT io_count int8, OUT others_append_count int8,
    OUT startup_append_count int8) FROM PUBLIC;

CREATE FUNCTION polar_xlog_buffer_stat_reset()
RETURNS VOID
AS 'MODULE_PATHNAME', 'polar_xlog_buffer_stat_reset'
LANGUAGE C PARALLEL SAFE;

REVOKE ALL ON FUNCTION polar_xlog_buffer_stat_reset() FROM PUBLIC;

CREATE FUNCTION polar_get_slot_node_type(slot_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'polar_get_slot_node_type'
LANGUAGE C PARALLEL SAFE;
