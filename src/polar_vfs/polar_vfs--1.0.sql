/* src/polar_vfs/polar_vfs--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_vfs" to load this file. \quit

CREATE TYPE mm_type AS (mem_type text, malloc_count int8, malloc_bytes int8, free_count int8, free_btyes int8);

CREATE FUNCTION polar_vfs_mem_status ()
RETURNS setof mm_type
AS 'MODULE_PATHNAME','polar_vfs_mem_status'
LANGUAGE C STRICT;

CREATE FUNCTION polar_vfs_disk_expansion (text)
RETURNS bool
AS 'MODULE_PATHNAME','polar_vfs_disk_expansion'
LANGUAGE C PARALLEL SAFE STRICT;

CREATE FUNCTION polar_libpfs_version ()
RETURNS text
AS 'MODULE_PATHNAME','polar_libpfs_version'
LANGUAGE C PARALLEL SAFE STRICT;
