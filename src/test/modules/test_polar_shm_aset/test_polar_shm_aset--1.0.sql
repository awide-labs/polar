/* src/test/modules/test_buffer/test_polar_shm_aset--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_polar_shm_aset" to load this file. \quit

CREATE FUNCTION test_shm_aset_random(loops int, num_allocs int, min_alloc int, max_alloc int, mode text)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE TYPE test_shm_aset_row AS (pid int, allocations bigint, elapsed interval);

CREATE FUNCTION test_shm_aset_random_parallel(loops int, num_allocs int, min_alloc int, max_alloc int, mode text, workers int)
RETURNS SETOF test_shm_aset_row
AS 'MODULE_PATHNAME'
LANGUAGE C;
