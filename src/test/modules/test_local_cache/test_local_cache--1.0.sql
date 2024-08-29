/* src/test/modules/test_local_cache/test_local_cache--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_local_cache" to load this file. \quit

CREATE FUNCTION test_local_cache()
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;
