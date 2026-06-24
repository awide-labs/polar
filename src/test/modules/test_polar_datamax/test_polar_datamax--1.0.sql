/* src/test/modules/test_polar_datamax/test_polar_datamax--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_polar_datamax" to load this file. \quit

CREATE FUNCTION test_polar_datamax()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
