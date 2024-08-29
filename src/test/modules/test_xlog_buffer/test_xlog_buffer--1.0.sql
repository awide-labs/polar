/* src/test/modules/test_xlog_buffer/test_xlog_buffer--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_xlog_buffer" to load this file. \quit

CREATE FUNCTION test_xlog_buffer()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
