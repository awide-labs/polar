/* src/test/modules/test_logindex/test_logindex--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_logindex" to load this file. \quit

CREATE FUNCTION test_bitpos()
RETURNS int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_ringbuf()
RETURNS int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_mini_trans()
RETURNS int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_logindex()
RETURNS int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_fullpage()
RETURNS int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_polar_rel_size_cache()
RETURNS int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_checkpoint_ringbuf()
RETURNS int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
