SET ROLE polar_repack_superuser;
CREATE SCHEMA repack_normal;
SET search_path TO repack_normal;
CREATE TABLE normal (
    a_int INT PRIMARY KEY,
    b_text TEXT,
    c_varchar VARCHAR(30),
    d_num NUMERIC
);
-- Load data
INSERT INTO normal SELECT i * 10, i, i, i FROM generate_series(0, 9) i;
-- Create indexes
CREATE INDEX normal_idx_btree_a on normal USING btree (a_int);
CREATE INDEX normal_idx_gin_b on normal USING gin (b_text);
CREATE INDEX normal_idx_gist_c on normal USING gist (c_varchar);

-- normal table
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_normal.normal --polar-dc-check
--- no consistency check when repacking only indexes, we only check table content now
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_normal.normal --polar-dc-check --only-indexes
--- no consistency check when repacking only indexes, we only check table content now
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_normal.normal_pkey --polar-dc-check
--- no consistency check when repacking only indexes, we only check table content now
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_normal.normal_idx_btree_a --polar-dc-check
--- no consistency check when repacking only indexes, we only check table content now
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_normal.normal_idx_gin_b --polar-dc-check
--- no consistency check when repacking only indexes, we only check table content now
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_normal.normal_idx_gist_c --polar-dc-check

DELETE FROM repack_normal.normal;
-- empty table
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_normal.normal --polar-dc-check
--- no consistency check when repacking only indexes, we only check table content now
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_normal.normal --polar-dc-check --only-indexes

-- partitioned table
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt --polar-dc-check
--- ERROR, community bug
-- \! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt --polar-dc-check --only-indexes

-- partition
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt_p_0 --polar-dc-check
--- no consistency check when repacking only indexes, we only check table content now
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt_p_0 --polar-dc-check --only-indexes
SELECT COALESCE(
    (SELECT setting <> 'ora' AS pg_mode FROM pg_settings WHERE name = 'polar_compatibility_mode'),
    True
) AS pg_mode; \gset
-- For Oracle mode, the prt_p_0 is partitioned but not a leaf partition. Due to the community bug https://github.com/reorg/pg_repack/issues/389,
-- we skip the test for it.
\if :pg_mode
    --- no consistency check when repacking only indexes, we only check table content now
    \! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_0_pkey --polar-dc-check
    --- no consistency check when repacking only indexes, we only check table content now
    \! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_0_a_int_idx --polar-dc-check
    --- no consistency check when repacking only indexes, we only check table content now
    \! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_0_b_text_idx --polar-dc-check
\endif

-- test --polar-ignore-dropped-objects
-- We cannot drop the table concurrently in regression test, so we just test this option here.
-- It will be tested in test framework.
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_normal.normal --polar-dc-check --polar-ignore-dropped-objects
