/* external/polar_monitor/polar_monitor--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION polar_monitor UPDATE to '1.1'" to load this file. \quit

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
