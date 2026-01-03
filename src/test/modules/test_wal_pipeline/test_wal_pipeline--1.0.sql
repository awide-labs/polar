/* src/test/modules/test_csn/test_csn--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_wal_pipeline" to load this file. \quit

CREATE FUNCTION test_wal_pipeline()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;