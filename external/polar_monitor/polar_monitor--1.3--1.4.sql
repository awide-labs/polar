/* external/polar_monitor/polar_monitor--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION polar_monitor UPDATE TO '1.4'" to load this file. \quit

-- POLAR: combined node metrics for ProxySQL monitoring (DB-PSI + applier + CPU)
CREATE FUNCTION polar_stat_node_metrics(
	OUT active_backends int,
	OUT wait_io_read int,
	OUT wait_io_write int,
	OUT wait_lock int,
	OUT wait_other int,
	OUT applier_records_parsed bigint,
	OUT applier_stall_spins bigint,
	OUT applier_stall_records bigint,
	OUT num_cpus int,
	OUT cpu_usage_usec bigint,
	OUT cpu_psi_some_avg10 float8)
RETURNS RECORD
AS 'MODULE_PATHNAME', 'polar_stat_node_metrics'
LANGUAGE C PARALLEL SAFE;

CREATE VIEW polar_stat_node_metrics AS
	SELECT * FROM polar_stat_node_metrics();

-- DataMax monitoring
CREATE FUNCTION polar_get_datamax_info(
	OUT min_received_timeline int4,
	OUT min_received_lsn pg_lsn,
	OUT last_received_timeline int4,
	OUT last_received_lsn pg_lsn,
	OUT last_valid_received_lsn pg_lsn,
	OUT clean_reserved_lsn pg_lsn,
	OUT force_clean bool)
RETURNS record
AS 'MODULE_PATHNAME', 'polar_get_datamax_info'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_set_datamax_reserved_lsn(IN reserved_lsn pg_lsn, IN force bool)
RETURNS bool
AS 'MODULE_PATHNAME', 'polar_set_datamax_reserved_lsn'
LANGUAGE C PARALLEL SAFE;

REVOKE ALL ON FUNCTION polar_get_datamax_info() FROM PUBLIC;
REVOKE ALL ON FUNCTION polar_set_datamax_reserved_lsn(pg_lsn, bool) FROM PUBLIC;
