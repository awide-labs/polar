/* external/polar_worker/polar_worker--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_csn" to load this file. \quit

CREATE OR REPLACE FUNCTION txid_snapshot_csn(snapshot txid_snapshot)
RETURNS int8
AS 'MODULE_PATHNAME', 'txid_snapshot_csn'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION txid_csn(xid int8)
RETURNS int8
AS 'MODULE_PATHNAME', 'txid_csn'
LANGUAGE C STRICT;
