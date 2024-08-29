/* src/test/modules/test_coredump_handler/test_coredump_handler--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_coredump_handler" to load this file. \quit

CREATE FUNCTION test_sigsegv()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_panic()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_backtrace()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_assert()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
