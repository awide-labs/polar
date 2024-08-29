/* src/test/modules/test_rbtree/test_bulk_read--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_polar_bulk_read" to load this file. \quit

CREATE FUNCTION polar_drop_relation_buffers(
	   IN relation text,
	   IN forkname text,
	   IN first_del_block int4)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME', 'polar_drop_relation_buffers'
LANGUAGE C;
