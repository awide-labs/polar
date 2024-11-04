-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_smgrperf" to load this file. \quit

CREATE FUNCTION polar_smgrperf_prepare(
    nblocks INT DEFAULT 131072)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION polar_smgrperf_cleanup()
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION polar_smgrperf_read(
    bs INT DEFAULT 1,
    begin_blkno INT DEFAULT 0,
    end_blkno INT DEFAULT 131072,
    sequential BOOLEAN DEFAULT TRUE)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION polar_smgrperf_write(
    bs INT DEFAULT 1,
    begin_blkno INT DEFAULT 0,
    end_blkno INT DEFAULT 131072,
    sequential BOOLEAN DEFAULT TRUE)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION polar_smgrperf_extend(
    bs INT DEFAULT 1)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION polar_smgrperf_nblocks(
    relnumber OID DEFAULT 1,
    nblocks_cached BOOLEAN DEFAULT FALSE,
    fd_cached BOOLEAN DEFAULT TRUE
) RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
