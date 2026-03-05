\getenv abs_srcdir PG_ABS_SRCDIR
\getenv abs_builddir PG_ABS_BUILDDIR
\set filename :abs_srcdir '/data/data2.csv'

-- Must be errors because currently DIRECT implemented only for CSV
COPY customer_copy_from FROM :'filename' WITH (FORMAT BINARY, DIRECT);
COPY customer_copy_from FROM :'filename' WITH (FORMAT TEXT, DIRECT);
COPY customer_copy_from FROM :'filename' WITH (FORMAT TEXT, DIRECT);
-- Must be error because BULKLOAD is used without DIRECT
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, BULKLOAD 'SKIP=2');
-- Must be error because of unknown BULKLOAD option
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'blablabla=1');
-- Must be error because the CSV file contains two headers
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT);

-- Must be ok: single-process direct load (MULTI_PROCESS=NO)
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO');
SELECT * FROM customer_copy_from ORDER BY c_id;

-- Must be ok: parallel writer (MULTI_PROCESS=YES)
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = YES');
SELECT * FROM customer_copy_from ORDER BY c_id;

-- ============================================================
-- Loader log: MULTI_PROCESS (default YES vs explicit NO)
-- ============================================================
-- WriterDumpParams() writes "MULTI_PROCESS = YES|NO" to LOGFILE.  Use fixed
-- absolute LOGFILE paths (required by pg_bulkload) under results/, then grep.
\set bulkload_mp_log_default 'PARSE_ERRORS=50,LOGFILE=' :abs_builddir '/results/copy_from_mp_default.log'
\set bulkload_mp_log_no 'PARSE_ERRORS=50,MULTI_PROCESS = NO,LOGFILE=' :abs_builddir '/results/copy_from_mp_no.log'
\! rm -f results/copy_from_mp_default.log results/copy_from_mp_no.log
-- Default DIRECT COPY (no MULTI_PROCESS in BULKLOAD): backend supplies MULTI_PROCESS=YES.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD :'bulkload_mp_log_default');
\! grep -F 'MULTI_PROCESS = YES' results/copy_from_mp_default.log
-- Explicit MULTI_PROCESS=NO in BULKLOAD.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD :'bulkload_mp_log_no');
\! grep -F 'MULTI_PROCESS = NO' results/copy_from_mp_no.log

-- Must be error: WAL_LOGGED without DIRECT
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, WAL_LOGGED true);

-- Must be ok: WRITER=BUFFERED via WAL_LOGGED (MULTI_PROCESS=NO)
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, WAL_LOGGED, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO');
SELECT * FROM customer_copy_from ORDER BY c_id;

-- Must be ok: WRITER=BUFFERED via WAL_LOGGED (MULTI_PROCESS=YES)
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, WAL_LOGGED, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = YES');
SELECT * FROM customer_copy_from ORDER BY c_id;

-- Must be error: duplicate WRITER (WAL_LOGGED implies WRITER=BUFFERED; do not set WRITER again in BULKLOAD)
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, WAL_LOGGED, BULKLOAD 'WRITER=BUFFERED');

-- ============================================================
-- COPY FROM DIRECT ... WHERE clause tests
-- ============================================================

-- Test: WHERE filters some rows (MULTI_PROCESS = YES).
-- c_d_id > 229 matches rows with c_d_id 230, 231, 232 (3 of 7 valid rows).
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = YES') WHERE c_d_id > 229;
SELECT c_id, c_d_id FROM customer_copy_from ORDER BY c_id;

-- Test: WHERE filters some rows (MULTI_PROCESS = NO).
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO') WHERE c_d_id > 229;
SELECT c_id, c_d_id FROM customer_copy_from ORDER BY c_id;

-- Test: WHERE matches a single row (MULTI_PROCESS = YES).
-- c_d_id = 224 matches only the DUP-2 row.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = YES') WHERE c_d_id = 224;
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- Test: WHERE matches no rows (MULTI_PROCESS = NO).
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO') WHERE c_d_id < 0;
SELECT count(*) AS where_no_match FROM customer_copy_from;

-- Test: WHERE matches all rows (MULTI_PROCESS = YES).
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = YES') WHERE c_d_id > 0;
SELECT count(*) AS where_all_match FROM customer_copy_from;

-- Test: WHERE with compound expression (MULTI_PROCESS = NO).
-- c_d_id >= 228 AND c_d_id <= 230 matches rows 228, 229, 230 (3 rows).
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO') WHERE c_d_id >= 228 AND c_d_id <= 230;
SELECT c_id, c_d_id FROM customer_copy_from ORDER BY c_id;

-- ============================================================
-- COPY FROM DIRECT: option mapping (BuildPgBulkloadOptions)
-- ============================================================
-- Tests that compatible COPY FROM options are correctly forwarded
-- to pg_bulkload, and incompatible ones produce clear errors.

-- --- Incompatible options must error ---

-- FREEZE is not supported in DIRECT mode.
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, FREEZE, BULKLOAD 'PARSE_ERRORS=50');

-- FORCE_NULL is not supported in DIRECT mode.
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, FORCE_NULL (c_first), BULKLOAD 'PARSE_ERRORS=50');

-- HEADER MATCH is not supported in DIRECT mode.
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, HEADER MATCH, BULKLOAD 'PARSE_ERRORS=50');

-- --- Compatible option: DELIMITER ---
-- Pipe-delimited file with a header line.
\set pipe_file :abs_srcdir '/data/copy_from_pipe.csv'
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'pipe_file' WITH (FORMAT CSV, DIRECT, DELIMITER '|', HEADER, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS=NO');
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- Same with MULTI_PROCESS=YES.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'pipe_file' WITH (FORMAT CSV, DIRECT, DELIMITER '|', HEADER, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS=YES');
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- --- Compatible option: HEADER (mapped to SKIP=1) ---
-- data2.csv has two header lines; HEADER skips exactly one, the second header
-- becomes a parse error (just like without HEADER but with SKIP=1).
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, HEADER, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS=NO');
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- --- Compatible option: NULL ---
\set null_file :abs_srcdir '/data/copy_from_custom_null.csv'
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'null_file' WITH (FORMAT CSV, DIRECT, NULL 'CUSTOM_NULL', BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS=NO');
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- --- Compatible option: QUOTE ---
\set quote_file :abs_srcdir '/data/copy_from_custom_quote.csv'
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'quote_file' WITH (FORMAT CSV, DIRECT, QUOTE '''', ESCAPE '''', BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS=NO');
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- --- Compatible option: ENCODING ---
-- Specify encoding explicitly (same as server encoding so no conversion,
-- but exercises the ENCODING mapping path).
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'null_file' WITH (FORMAT CSV, DIRECT, ENCODING 'UTF8', BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS=NO');
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- --- Combined options: DELIMITER + HEADER + ENCODING ---
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'pipe_file' WITH (FORMAT CSV, DIRECT, DELIMITER '|', HEADER, ENCODING 'UTF8', BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS=NO');
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- ============================================================
-- Progress reporting for COPY FROM DIRECT (pg_stat_progress_copy)
-- ============================================================
-- DIRECT loads bypass heap inserts, so statement-level AFTER INSERT triggers
-- do not run.  Like the core regression (copy.sql), we report pg_stat_progress_copy
-- via RAISE INFO, but the snapshot is taken from a VOLATILE helper used in the
-- COPY WHERE clause so ExecQual runs it once per successfully parsed tuple
-- (before that tuple updates tuples_processed / tuples_excluded counters).
--
-- Why pg_stat_clear_snapshot() before each read (see also src/test/regress/sql/stats.sql):
--   SQL-accessible views such as pg_stat_progress_copy are backed by
--   pg_stat_get_progress_info() and friends.  Those functions read backend status
--   through pgstat_read_current_status() in backend_status.c.  On the *first*
--   access to any pg_stat_* data in the current transaction, that routine copies
--   the shared-memory PgBackendStatus array (including st_progress_param, where
--   COPY progress counters live) into a local cache (localBackendStatusTable).
--   On *subsequent* calls in the same transaction, pgstat_read_current_status()
--   returns immediately if the cache already exists ("already done"), so every
--   further query of pg_stat_progress_copy sees the *same frozen* tuple counts
--   as the first read — not the live values updated by pgstat_progress_update_param()
--   in ReaderNext() between tuples.
--   Calling pg_stat_clear_snapshot() discards that cache (pgstat_clear_backend_activity_snapshot
--   sets localBackendStatusTable to NULL), so the next SELECT from pg_stat_progress_copy
--   forces a fresh copy of the current counters.  Without this, all INFO lines
--   during one COPY would repeat the initial tuples_processed / tuples_excluded.
--   The core copy.sql progress test does not need this because it reads progress
--   only once per COPY (statement-level trigger).
--
-- Filter logic is combined with the snapshot in one function so the planner cannot
-- short-circuit past the snapshot (unlike  notice_fn() AND (expr)  where AND might
-- skip evaluating the left side).

CREATE FUNCTION notice_copy_from_direct_progress(c_d_id int, mode text) RETURNS boolean AS
$$
DECLARE
	report jsonb;
	pass boolean;
BEGIN
	/*
	 * Must run before querying pg_stat_progress_copy; see block comment above.
	 */
	PERFORM pg_stat_clear_snapshot();
	WITH progress_data AS (
		SELECT
			relid::regclass::text AS relname,
			command,
			type,
			bytes_processed > 0 AS has_bytes_processed,
			bytes_total > 0 AS has_bytes_total,
			tuples_processed,
			tuples_excluded
		FROM pg_stat_progress_copy
		WHERE pid = pg_backend_pid())
	SELECT to_jsonb(r) INTO report FROM progress_data r;

	RAISE INFO 'progress: %', report::text;

	IF mode = 'all' THEN
		pass := (c_d_id IS NOT NULL);
	ELSIF mode = 'gt229' THEN
		pass := (c_d_id > 229);
	ELSE
		RAISE EXCEPTION 'invalid mode %', mode;
	END IF;
	RETURN pass;
END;
$$ LANGUAGE plpgsql;
-- Full load (all successfully parsed tuples pass the filter): monotonic tuples_processed, MULTI_PROCESS = NO.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO') WHERE notice_copy_from_direct_progress(c_d_id, 'all');
SELECT count(*) AS loaded_all FROM customer_copy_from;

-- WHERE rejects some rows: tuples_excluded and tuples_processed match core COPY semantics, MULTI_PROCESS = NO.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO') WHERE notice_copy_from_direct_progress(c_d_id, 'gt229');
SELECT c_id, c_d_id FROM customer_copy_from ORDER BY c_id;

-- Full load (all successfully parsed tuples pass the filter): monotonic tuples_processed, MULTI_PROCESS = YES.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = YES') WHERE notice_copy_from_direct_progress(c_d_id, 'all');
SELECT count(*) AS loaded_all FROM customer_copy_from;

-- WHERE rejects some rows: tuples_excluded and tuples_processed match core COPY semantics, MULTI_PROCESS = YES.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM :'filename' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = YES') WHERE notice_copy_from_direct_progress(c_d_id, 'gt229');
SELECT c_id, c_d_id FROM customer_copy_from ORDER BY c_id;

-- ============================================================
-- COPY FROM DIRECT: stdin (client / libpq copy-in protocol)
-- ============================================================
-- Single-row load, single-process reader (INPUT=stdin in pg_bulkload).
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM stdin WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO');
0016777224,0224,0,DUP-2           ,AA,AAAAAAAAAAAAAAAA,c_street_1          ,c_street_2          ,AAAAAAAAAAAAAAAAAAAA,AA,AAAAAAAAA,AAAAAAAAAAAAAAAA,2006-01-01 12:34:56,AA,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,123456789012345678
\.
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- Two rows, multi-process writer.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM stdin WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = YES');
0016777227,0227,0,ABCDEFG         ,AA,AAAAAAAAAAAAAAAA,c_street_1          ,c_street_2          ,AAAAAAAAAAAAAAAAAAAA,AA,AAAAAAAAA,AAAAAAAAAAAAAAAA,2006-01-01 12:34:56,AA,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,123456789012345678
0016777228,0228,0,ABCDEFG         ,AA,AAAAAAAAAAAAAAAA,c_street_1          ,c_street_2          ,AAAAAAAAAAAAAAAAAAAA,AA,AAAAAAAAA,AAAAAAAAAAAAAAAA,2006-01-01 12:34:56,AA,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,123456789012345678
\.
SELECT c_id, c_d_id, c_first FROM customer_copy_from ORDER BY c_id;

-- WHERE on stdin-fed DIRECT load (no progress INFO).
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM stdin WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO') WHERE c_d_id > 228;
0016777227,0227,0,ABCDEFG         ,AA,AAAAAAAAAAAAAAAA,c_street_1          ,c_street_2          ,AAAAAAAAAAAAAAAAAAAA,AA,AAAAAAAAA,AAAAAAAAAAAAAAAA,2006-01-01 12:34:56,AA,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,123456789012345678
0016777230,0230,0,ABCDEFG         ,AA,AAAAAAAAAAAAAAAA,c_street_1          ,c_street_2          ,AAAAAAAAAAAAAAAAAAAA,AA,AAAAAAAAA,AAAAAAAAAAAAAAAA,2006-01-01 12:34:56,AA,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,123456789012345678
\.
SELECT c_id, c_d_id FROM customer_copy_from ORDER BY c_id;

-- BULKLOAD SKIP=2 with two header lines in the copy stream.
TRUNCATE customer_copy_from;
COPY customer_copy_from FROM stdin WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO,SKIP=2');
HEADER1
HEADER2
0016777224,0224,0,DUP-2           ,AA,AAAAAAAAAAAAAAAA,c_street_1          ,c_street_2          ,AAAAAAAAAAAAAAAAAAAA,AA,AAAAAAAAA,AAAAAAAAAAAAAAAA,2006-01-01 12:34:56,AA,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,12345.6789,123456789012345678
\.
SELECT count(*) AS stdin_skip2_rows FROM customer_copy_from;

-- ============================================================
-- COPY FROM DIRECT: PROGRAM (not supported; pg_bulkload INPUT is file or stdin)
-- ============================================================
\set program_csv 'cat ' :'filename'
COPY customer_copy_from FROM PROGRAM :'program_csv' WITH (FORMAT CSV, DIRECT, BULKLOAD 'PARSE_ERRORS=50,MULTI_PROCESS = NO');

DROP FUNCTION notice_copy_from_direct_progress(int, text);
