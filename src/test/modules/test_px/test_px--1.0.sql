/* src/test/modules/test_px/test_px--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_px" to load this file. \quit

CREATE FUNCTION test_px()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

-- dry run a sql
CREATE FUNCTION test_px_gpopt_sql(sql cstring)
RETURNS void STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- convert sql into a dxl
CREATE FUNCTION test_px_gpopt_sql_to_dxl(sql cstring)
RETURNS text STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- test all wrapper function
CREATE FUNCTION test_px_gpopt_wrapper()
RETURNS void STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- test all config param when run a sql
CREATE FUNCTION test_px_gpopt_config_param(sql cstring)
RETURNS void STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- run a sql in all cost model param
CREATE FUNCTION test_px_gpopt_cost_module(sql cstring)
RETURNS void STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- get child index oid
CREATE FUNCTION test_px_partition_child_list(oid)
RETURNS text STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- is rel a partition table
CREATE FUNCTION test_px_partition_is_partition_rel(oid)
RETURNS bool STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- is index a partition table
CREATE FUNCTION test_px_partition_is_partition_index(oid)
RETURNS bool STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION test_px_partition_funcs(relid oid, is_rel_partitioned bool)
RETURNS bool STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;
