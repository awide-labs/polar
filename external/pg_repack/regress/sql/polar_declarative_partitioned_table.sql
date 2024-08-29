/*
 * polar_declarative_partitioned_table.sql
 *
 * Test declarative partitioned table because the cases from community
 * only test inherited tables.
 *
 * This case can run in any branch because it doesn't include Oracle grammar and global index
 */

SET ROLE polar_repack_superuser;
CREATE SCHEMA repack_part_schema;
SET search_path TO repack_part_schema;
-- Create partitioned table
CREATE TABLE prt (
    a_int INT PRIMARY KEY,
    b_text TEXT,
    c_varchar VARCHAR(30),
    d_num NUMERIC
) PARTITION BY RANGE (a_int);

-- Create 3 range partitions
CREATE TABLE prt_p_0 PARTITION OF prt FOR VALUES FROM (0) TO (10);
CREATE TABLE prt_p_1 PARTITION OF prt FOR VALUES FROM (10) TO (20);
CREATE TABLE prt_p_2 PARTITION OF prt FOR VALUES FROM (20) TO (30);

-- Create default partition, which is also partitioned
CREATE TABLE prt_p_def PARTITION OF prt DEFAULT PARTITION BY HASH (a_int);
-- Create 3 hash sub-partitions in default partition
CREATE TABLE prt_p_def_p_0 PARTITION OF prt_p_def FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE prt_p_def_p_1 PARTITION OF prt_p_def FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE "prt_p_def_P_2" PARTITION OF prt_p_def FOR VALUES WITH (MODULUS 3, REMAINDER 2);

-- Load data
INSERT INTO prt SELECT i * 10, i, i, i FROM generate_series(0, 9) i;
-- Create indexes
CREATE INDEX prt_idx_btree_a on prt USING btree (a_int);
CREATE INDEX prt_idx_gin_b on prt USING gin (b_text);
CREATE INDEX prt_idx_gist_c on prt USING gist (c_varchar);
CREATE INDEX "prt_idx_HASH_d" on prt USING hash (d_num);

-- show table definition
\d+ prt
\d+ prt_p_0
\d+ prt_p_def
\d prt_p_def_p_0

-- Force index scan to prove the index is ok after repacking
SET enable_seqscan TO off;

--
-- 1. Repack partitioned table
--
SELECT count(*) FROM prt;
--- ERROR, cannot use --table for partitioned table
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt
--- OK, can only use --parent-table for partitioned table
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt
SELECT count(*) FROM prt;

--
-- 2. Repack partition
--
SELECT * FROM prt_p_0;
SELECT * FROM prt_p_1;
SELECT * FROM prt_p_2;
--- OK to use --table for partition
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_0
--- OK to use --parent-table for partition
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt_p_0
--- OK for multi partitions
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_0 --table=repack_part_schema.prt_p_1 --table=repack_part_schema.prt_p_2
SELECT * FROM prt_p_0;
SELECT * FROM prt_p_1;
SELECT * FROM prt_p_2;
-- Scan by index, prove that index is ok after repacking partition
SELECT * FROM prt WHERE a_int = 0;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE a_int = 0;
SELECT * FROM prt WHERE b_text = '0';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '0';
SELECT * FROM prt WHERE c_varchar = '0';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '0';
SELECT * FROM prt WHERE d_num = 0;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE d_num = 0;

--
-- 3. Repack partitioned partition
--
SELECT count(*) FROM prt_p_def;
--- ERROR, cannot use --table for partitioned partition
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def
--- OK, can only use --parent-table for partitioned partition
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt_p_def
--- OK, partitioned table and partitioned partition
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt --parent-table=repack_part_schema.prt_p_def
--- OK, partitioned table and sub-partition
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt --table=repack_part_schema.prt_p_def_p_1
SELECT count(*) FROM prt_p_def;
-- Scan by index, prove that index is ok after repacking partitioned partition
SELECT * FROM prt WHERE a_int = 50;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE a_int = 50;
SELECT * FROM prt WHERE b_text = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '5';
SELECT * FROM prt WHERE c_varchar = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '5';
SELECT * FROM prt WHERE d_num = 5;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE d_num = 5;

--
-- 4. Repack sub-partition
--
SELECT count(*) FROM prt_p_def;
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_1
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt_p_def_p_1
--- OK for multi sub-partition
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_0 --table=repack_part_schema.prt_p_def_p_1 --table='repack_part_schema."prt_p_def_P_2"'
--- Error, upper case name should be preserved by '' and ""
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_0 --table=repack_part_schema.prt_p_def_p_1 --table=repack_part_schema.prt_p_def_P_2
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_0 --table=repack_part_schema.prt_p_def_p_1 --table="repack_part_schema.prt_p_def_P_2"
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_0 --table=repack_part_schema.prt_p_def_p_1 --table=repack_part_schema."prt_p_def_P_2"
SELECT count(*) FROM prt_p_def;
-- Scan by index, prove that index is ok after repacking leaf partition
SELECT * FROM prt WHERE a_int = 50;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE a_int = 50;
SELECT * FROM prt WHERE b_text = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '5';
SELECT * FROM prt WHERE c_varchar = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '5';
SELECT * FROM prt WHERE d_num = 5;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE d_num = 5;

--
-- 5. Repack index on partitioned table
--
SELECT count(*) FROM prt;
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt --only-indexes

-- This is not supported and not planned to be supported by community
-- because repacking index is deprecated and should be replaced by REINDEX CONCURRENTLY in PG12+ versions
-- see issue for more details: https://github.com/reorg/pg_repack/issues/389
-- \! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_pkey

SELECT count(*) FROM prt;
-- Scan by index, prove that index is ok after repacking index on partition
SELECT * FROM prt WHERE a_int = 0;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE a_int = 0;
SELECT * FROM prt WHERE b_text = '0';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '0';
SELECT * FROM prt WHERE c_varchar = '0';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '0';
SELECT * FROM prt WHERE d_num = 0;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE d_num = 0;

--
-- 6. Repack index on partition
--
SELECT count(*) FROM prt_p_0;
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_0 --only-indexes
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt_p_0 --only-indexes
--- OK for multi partitions
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_0 --table=repack_part_schema.prt_p_1 --table=repack_part_schema.prt_p_2 --only-indexes
--- OK for primary key index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_0_pkey
--- OK for btree index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_0_a_int_idx
--- OK for gin index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_0_b_text_idx
--- OK for gist index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_0_c_varchar_idx
--- OK for multi index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_0_pkey --index=repack_part_schema.prt_p_0_a_int_idx --index=repack_part_schema.prt_p_0_b_text_idx --index=repack_part_schema.prt_p_0_c_varchar_idx
SELECT count(*) FROM prt_p_0;
-- Scan by index, prove that index is ok after repacking index on partition
SELECT * FROM prt WHERE a_int = 0;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE a_int = 0;
SELECT * FROM prt WHERE b_text = '0';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '0';
SELECT * FROM prt WHERE c_varchar = '0';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '0';
SELECT * FROM prt WHERE d_num = 0;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE d_num = 0;

--
-- 7. Repack index on partitioned partition
--
SELECT count(*) FROM prt_p_def;
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt_p_def --only-indexes

-- This is not supported and not planned to be supported by community
-- because repacking index is deprecated and should be replaced by REINDEX CONCURRENTLY in PG12+ versions
-- see issue for more details: https://github.com/reorg/pg_repack/issues/389
-- \! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_def_pkey

SELECT count(*) FROM prt_p_def;
-- Scan by index, prove that index is ok after repacking index on partitioned partition
SELECT * FROM prt WHERE a_int = 50;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE a_int = 50;
SELECT * FROM prt WHERE b_text = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '5';
SELECT * FROM prt WHERE c_varchar = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '5';
SELECT * FROM prt WHERE d_num = 5;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE d_num = 5;

--
-- 8. Repack index on sub-partition
--
SELECT count(*) FROM prt_p_def;
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_0 --only-indexes
--- OK
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt_p_def_p_0 --only-indexes
--- OK for multi sub-partition
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_0 --table=repack_part_schema.prt_p_def_p_1 --table='repack_part_schema."prt_p_def_P_2"' --only-indexes
--- Error, upper case name should be preserved by '' and ""
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_0 --table=repack_part_schema.prt_p_def_p_1 --table=repack_part_schema.prt_p_def_P_2 --only-indexes
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_0 --table=repack_part_schema.prt_p_def_p_1 --table="repack_part_schema.prt_p_def_P_2" --only-indexes
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_part_schema.prt_p_def_p_0 --table=repack_part_schema.prt_p_def_p_1 --table=repack_part_schema."prt_p_def_P_2" --only-indexes
--- OK for primary key index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_def_p_0_pkey
--- OK for btree index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_def_p_0_a_int_idx
--- OK for gin index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_def_p_0_b_text_idx
--- OK for gist index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_def_p_0_c_varchar_idx
--- OK for multi index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_part_schema.prt_p_def_p_0_pkey --index=repack_part_schema.prt_p_def_p_0_a_int_idx --index=repack_part_schema.prt_p_def_p_0_b_text_idx --index=repack_part_schema.prt_p_def_p_0_c_varchar_idx
-- Scan by index, prove that index is ok after repacking index on leaf partition
SELECT * FROM prt WHERE a_int = 50;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE a_int = 50;
SELECT * FROM prt WHERE b_text = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '5';
SELECT * FROM prt WHERE c_varchar = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '5';
SELECT * FROM prt WHERE d_num = 5;
EXPLAIN (COSTS off) SELECT * FROM prt WHERE d_num = 5;

SELECT count(*) FROM prt_p_def;
RESET search_path;
