/* external/polar_worker/polar_worker--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_worker" to load this file. \quit

CREATE FUNCTION test_read_core_pattern()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;