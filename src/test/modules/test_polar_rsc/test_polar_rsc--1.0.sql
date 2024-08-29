/* src/test/modules/test_polar_rsc/test_polar_rsc--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_polar_rsc" to load this file. \quit

CREATE FUNCTION test_polar_rsc_stat_entries(
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
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_polar_rsc_search_by_ref(IN oid)
RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_polar_rsc_search_by_mapping(IN oid)
RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_polar_rsc_update_entry(IN oid)
RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C;
