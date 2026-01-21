/* src/test/modules/test_csn_xact/test_csn_xact--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_csn_xact" to load this file. \quit

CREATE FUNCTION test_csn_xact_xmin(bool)
RETURNS void
AS 'MODULE_PATHNAME', 'test_csn_xact_xmin'
LANGUAGE C STRICT;

CREATE FUNCTION test_csn_xact_xmax(bool)
RETURNS void
AS 'MODULE_PATHNAME', 'test_csn_xact_xmax'
LANGUAGE C STRICT;

CREATE FUNCTION test_csn_xact_multixact(bool)
RETURNS void
AS 'MODULE_PATHNAME', 'test_csn_xact_multixact'
LANGUAGE C STRICT;
