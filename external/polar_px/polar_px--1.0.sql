-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_px" to load this file. \quit

CREATE OR REPLACE FUNCTION polar_px_workerid()
RETURNS int4
AS 'MODULE_PATHNAME', 'polar_px_workerid'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION polar_px_workerid_funcid()
RETURNS int4
AS 'MODULE_PATHNAME', 'polar_px_workerid_funcid'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION enable_xform(text)
RETURNS TEXT STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE OR REPLACE FUNCTION disable_xform(text)
RETURNS TEXT STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
