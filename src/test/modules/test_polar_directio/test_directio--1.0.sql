/* src/test/modules/test_polar_directio/test_polar_directio--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_directio" to load this file. \quit

CREATE FUNCTION test_directio()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
