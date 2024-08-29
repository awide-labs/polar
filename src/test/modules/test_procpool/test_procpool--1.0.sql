/* src/test/modules/test_procpool/test_procpool--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_parallel_redo" to load this file. \quit

CREATE FUNCTION test_procpool(INTEGER)
RETURNS bigint 
AS 'MODULE_PATHNAME' LANGUAGE C;
