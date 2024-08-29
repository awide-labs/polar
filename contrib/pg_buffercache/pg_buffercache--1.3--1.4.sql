/* contrib/pg_buffercache/pg_buffercache--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_bufferache" to load this file. \quit

CREATE FUNCTION polar_drop_relation_buffers(
	   IN relation text,
	   IN forkname text)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME', 'polar_drop_relation_buffers'
LANGUAGE C;
