/* external/polar_monitor/polar_monitor_preload--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_monitor_preload" to load this file. \quit

/* POLAR: memory context status */
CREATE FUNCTION polar_get_mcxt(
    IN pid int4,
    OUT pid int4,
    OUT name text,
    OUT level int4,
    OUT nblocks int8,
    OUT freechunks int8,
    OUT totalspace int8,
    OUT freespace int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_get_memory_stats'
LANGUAGE C IMMUTABLE;

/* POLAR: local memory context status */
CREATE FUNCTION polar_get_local_mcxt(
        OUT pid int4,
        OUT name text,
        OUT level int4,
        OUT nblocks int8,
        OUT freechunks int8,
        OUT totalspace int8,
        OUT freespace int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_get_local_memory_stats'
LANGUAGE C IMMUTABLE;

/* POLAR: memory context record type */
CREATE TYPE polar_mcxt_record AS (
    pid int,
    name text,
    nblocks int8,
    freechunks int8,
    totalspace int8,
    freespace int8
);

/* POLAR: UNION ALL memory context from all backends together */
CREATE OR REPLACE FUNCTION polar_backends_mcxt()
RETURNS SETOF polar_mcxt_record
AS
$x$
DECLARE
    sql text;
    selfpid int;
    p int;
BEGIN
    selfpid := (SELECT PG_BACKEND_PID());
    /* just a mock schema */
    sql := '(SELECT 0 AS pid, ' || quote_nullable('hello') || ' AS name, '
      || ' 0::BIGINT AS nblocks, 0::BIGINT AS freechunks, '
      || ' 0::BIGINT AS totalspace, 0::BIGINT  AS freespace LIMIT 0)';

    FOR p IN SELECT pid FROM pg_stat_activity WHERE backend_type='client backend'
    LOOP
    sql := sql || ' UNION ALL '
        || 'SELECT pid, name, '
        || 'SUM(nblocks)::BIGINT AS nblocks, SUM(freechunks)::BIGINT AS freechunks, '
        || 'SUM(totalspace)::BIGINT AS totalspace, SUM(freespace)::BIGINT AS freespace ';
    IF selfpid != p THEN
        sql := sql || 'FROM polar_get_mcxt(' || p || ') ';
    ELSE
        sql := sql || 'FROM polar_get_local_mcxt()';
    END IF;

    sql := sql || ' GROUP BY pid, name ';

    END LOOP;

    sql := 'SELECT pid, name, nblocks, freechunks, totalspace, freespace FROM ( ' || sql || ') t';

    RETURN QUERY EXECUTE sql;
END
$x$
LANGUAGE plpgsql;

CREATE VIEW polar_backends_mcxt AS
    SELECT * FROM polar_backends_mcxt() ORDER BY totalspace DESC, pid;

/*
 * polar_stat_lwlock
 */
CREATE FUNCTION polar_stat_lwlock
(
    OUT tranche int2,
    OUT name text,
    OUT sh_acquire_count int8,
    OUT ex_acquire_count int8,
    OUT block_count int8,
    OUT wait_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_stat_lwlock'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/*
 * POLAR:
 *    lwlock wait nums for each lwlock
 */
CREATE FUNCTION polar_lwlock_stat_waiters
(
    OUT tranche int2,
    OUT lock_waiters int4,
    OUT sh_nums boolean,
    OUT ex_nums boolean
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_lwlock_stat_waiters'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE VIEW polar_lwlock_stat_waiters AS
    SELECT tranche, count(lock_waiters) lock_nums, max(lock_waiters) max_lock_waiters, cast (avg(lock_waiters) as float) avg_lock_waiters, sum(cast (sh_nums as int)) sh_nums, sum(cast (ex_nums as int)) ex_nums FROM polar_lwlock_stat_waiters() GROUP BY tranche ORDER BY tranche ASC;

CREATE VIEW polar_stat_lwlock AS
    SELECT a.*, b.lock_nums, b.max_lock_waiters,  b.avg_lock_waiters, b.sh_nums, b.ex_nums FROM polar_stat_lwlock() a left JOIN polar_lwlock_stat_waiters b on a.tranche=b.tranche ORDER BY tranche ASC;

/*
 * POLAR:
 *    lwlock stat for each backend
 */
CREATE FUNCTION polar_proc_stat_lwlock
(
    OUT pid int4,
    OUT tranche int2,
    OUT name text,
    OUT sh_acquire_count int8,
    OUT ex_acquire_count int8,
    OUT block_count int8,
    OUT wait_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_proc_stat_lwlock'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/*
 * polar_stat_lock
 */
CREATE FUNCTION polar_stat_lock
(
    OUT id int4,
    OUT lock_type text,
    OUT lock_mode text,
    OUT lock_count int8,
    OUT block_count int8,
    OUT fastpath_count int8,
    OUT wait_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_stat_lock'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE VIEW polar_stat_lock AS
    SELECT
        id AS id
        , lock_type AS lock_type
        , SUM(CASE WHEN lock_mode='INVALID' THEN lock_count ELSE 0 END) AS INVALID
        , SUM(CASE WHEN lock_mode='AccessShareLock' THEN lock_count ELSE 0 END) AS AccessShareLock
        , SUM(CASE WHEN lock_mode='RowShareLock' THEN lock_count ELSE 0 END) AS RowShareLock
        , SUM(CASE WHEN lock_mode='RowExclusiveLock' THEN lock_count ELSE 0 END) AS RowExclusiveLock
        , SUM(CASE WHEN lock_mode='ShareUpdateExclusiveLock' THEN lock_count ELSE 0 END) AS ShareUpdateExclusiveLock
        , SUM(CASE WHEN lock_mode='ShareLock' THEN lock_count ELSE 0 END) AS ShareLock
        , SUM(CASE WHEN lock_mode='ShareRowExclusiveLock' THEN lock_count ELSE 0 END) AS ShareRowExclusiveLock
        , SUM(CASE WHEN lock_mode='ExclusiveLock' THEN lock_count ELSE 0 END) AS ExclusiveLock
        , SUM(CASE WHEN lock_mode='AccessExclusiveLock' THEN lock_count ELSE 0 END) AS AccessExclusiveLock
        , SUM(block_count) AS block_count
        , SUM(fastpath_count) AS fastpath_count
        , SUM(wait_time) AS wait_time
    FROM
        polar_stat_lock()
    GROUP BY
        id,
        lock_type
    ORDER BY
        id;

/*
 * POLAR:
 *    lock stat for each backend
 */
CREATE FUNCTION polar_proc_stat_lock
(
    OUT pid int4,
    OUT id int4,
    OUT lock_type text,
    OUT lock_mode text,
    OUT lock_count int8,
    OUT block_count int8,
    OUT fastpath_count int8,
    OUT wait_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_proc_stat_lock'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/*
 * polar_stat_network
 */
CREATE FUNCTION polar_stat_network
(
    OUT send_count int8,
    OUT send_bytes int8,
    OUT send_block_time int8,
    OUT recv_count int8,
    OUT recv_bytes int8,
    OUT recv_block_time int8,
    OUT retrans int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_stat_network'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE VIEW polar_stat_network AS
    SELECT * FROM polar_stat_network();

/*
 * POLAR:
 *    network stat for each backend
 */
CREATE FUNCTION polar_proc_stat_network
(
    OUT pid int4,
    OUT send_count int8,
    OUT send_bytes int8,
    OUT send_block_time int8,
    OUT recv_count int8,
    OUT recv_bytes int8,
    OUT recv_block_time int8,
    OUT sendq int8,
    OUT recvq int8,
    OUT cwnd int8,
    OUT rtt int8,
    OUT rttvar int8,
    OUT retrans int8,
    OUT tcpinfo_update_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'polar_proc_stat_network'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
