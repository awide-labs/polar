/* src/test/modules/test_slru/test_slru--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_slru" to load this file. \quit

CREATE FUNCTION test_slru()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_slru_hash_index(INTEGER, INTEGER, INTEGER)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_slru_slot_size_config()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
