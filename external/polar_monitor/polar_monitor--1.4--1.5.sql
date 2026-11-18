/* external/polar_monitor/polar_monitor--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION polar_monitor UPDATE TO '1.5'" to load this file. \quit

-- cumulative replay-wait metrics for ProxySQL profiling
CREATE FUNCTION polar_stat_proxy_lsn_wait(
	OUT metric text,
	OUT value bigint)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'polar_stat_proxy_lsn_wait'
LANGUAGE C PARALLEL SAFE;
