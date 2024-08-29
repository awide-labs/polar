/* src/test/modules/test_buffer/test_buffer--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_buffer" to load this file. \quit

CREATE FUNCTION test_buffer()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
