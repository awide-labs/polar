/*
 * polar_global_index.sql
 *
 * Test global index.
 */

SET ROLE polar_repack_superuser;
CREATE SCHEMA repack_gi_schema;
SET search_path TO repack_gi_schema;

-- Create partitioned table
CREATE TABLE prt (
    a_int INT PRIMARY KEY,
    b_text TEXT,
    c_varchar VARCHAR(30),
    d_num NUMERIC
) PARTITION BY RANGE (a_int);
-- Create normal table
CREATE TABLE normal (
    a_int INT PRIMARY KEY,
    b_text TEXT,
    c_varchar VARCHAR(30),
    d_num NUMERIC
);

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
INSERT INTO normal SELECT i * 10, i, i, i FROM generate_series(0, 9) i;

-- Create global indexes on partitioned table (even in pg mode)
CREATE INDEX prt_gi_btree_b ON prt USING btree (b_text) GLOBAL;
CREATE INDEX "prt_gi_BTREE_c" ON prt USING btree (c_varchar) GLOBAL;

-- GPI is not supported now
CREATE INDEX prt_gpi_btree_d ON prt USING btree (d_num) GLOBAL PARTITION BY RANGE (d_num)
(
    PARTITION prt_gpi_btree_d_p_0 VALUES less than(0),
    PARTITION prt_gpi_btree_d_p_1 VALUES less than(10),
    PARTITION prt_gpi_btree_d_p_max VALUES less than(maxvalue)
);

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
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt
--- ERROR, does not support repack table with global index by default
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt
--- OK, can only use --parent-table for partitioned table
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt
--- OK, use --parent-table for normal table
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.normal
--- OK, mix normal table and partitioned table
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt --table=repack_gi_schema.normal
SELECT count(*) FROM prt;

--
-- 2. Repack partition
--
SELECT count(*) FROM prt_p_0;
SELECT count(*) FROM prt_p_1;
SELECT count(*) FROM prt_p_2;
--- ERROR, does not support repack table with global index by default
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_0
--- OK to use --table for partition
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_0
--- OK to use --parent-table for partition
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_0
--- OK to specify duplicate partition name
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_0 --parent-table=repack_gi_schema.prt_p_0
--- OK for multi partitions
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_0 --table=repack_gi_schema.prt_p_1 --table=repack_gi_schema.prt_p_2
--- OK to mix partition and partitioned table
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt --table=repack_gi_schema.prt_p_0
--- OK to mix partition and normal table
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.normal --table=repack_gi_schema.prt_p_0
SELECT count(*) FROM prt_p_0;
SELECT count(*) FROM prt_p_1;
SELECT count(*) FROM prt_p_2;
-- Scan by index, prove that index is ok after repacking 1 partition
SELECT * FROM prt WHERE b_text = '1';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '1';
SELECT * FROM prt WHERE c_varchar = '1';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '1';

--
-- 3. Repack partitioned partition
--
SELECT count(*) FROM prt_p_def;
--- ERROR, cannot use --table for partitioned partition
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def
--- ERROR, does not support repack table with global index by default
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_def
--- OK, can only use --parent-table for partitioned partition
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_def
--- OK, partitioned table and partitioned partition
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt --parent-table=repack_gi_schema.prt_p_def
--- OK, partitioned partition and sub-partition
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_def --table=repack_gi_schema.prt_p_def_p_1
--- OK, partitioned partition and normal table
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_def --table=repack_gi_schema.normal
SELECT count(*) FROM prt_p_def;
-- Scan by index, prove that index is ok after repacking 1 partition
SELECT * FROM prt WHERE b_text = '2';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '2';
SELECT * FROM prt WHERE c_varchar = '2';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '2';

--
-- 4. Repack sub-partition
--
SELECT count(*) FROM prt_p_def;
--- ERROR, does not support repack table with global index by default
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_1
--- OK
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_1
--- OK
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_def_p_1
--- OK for multi sub-partition
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --table=repack_gi_schema.prt_p_def_p_1 --table='repack_part_schema."prt_p_def_P_2"'
--- Error, upper case name should be preserved by '' and ""
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --table=repack_gi_schema.prt_p_def_p_1 --table=repack_gi_schema.prt_p_def_P_2
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --table=repack_gi_schema.prt_p_def_p_1 --table="repack_gi_schema.prt_p_def_P_2"
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --table=repack_gi_schema.prt_p_def_p_1 --table=repack_gi_schema."prt_p_def_P_2"
SELECT count(*) FROM prt_p_def;
-- Scan by index, prove that index is ok after repacking 1 partition
SELECT * FROM prt WHERE b_text = '3';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '3';
SELECT * FROM prt WHERE c_varchar = '3';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '3';

--
-- 5. Repack index on partitioned table
--
SELECT count(*) FROM prt_p_def;
--- OK for parent table. Only normal indexes are repacked by default, global indexes are skipped
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt --only-indexes
--- OK for parent table, global indexes are also repacked with --polar-enable-global-index
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt --only-indexes
--- Error, global index is not enabled by default
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_gi_btree_b --index='repack_gi_schema."prt_gi_BTREE_c"'
--- OK for global index with --polar-enable-global-index
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_gi_btree_b --index='repack_gi_schema."prt_gi_BTREE_c"'
--- Error, upper case name should be preserved by '' and ""
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_gi_BTREE_c
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index="repack_gi_schema.prt_gi_BTREE_c"
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema."prt_gi_BTREE_c"
--- Error for global partitioned index by default
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_gpi_btree_d
--- OK for global partitioned index with --polar-enable-global-index
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_gpi_btree_d
--- OK for global index and global partitioned index
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_gi_btree_b --index=repack_gi_schema.prt_gpi_btree_d
SELECT count(*) FROM prt_p_def;
-- Scan by index, prove that index is ok after repacking 1 partition
SELECT * FROM prt WHERE b_text = '3';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '3';
SELECT * FROM prt WHERE c_varchar = '3';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '3';


--
-- 6. Repack index on partition
--
SELECT count(*) FROM prt_p_0;
--- OK, but global indexes are not repacked since they are on parent table but not partitions
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_0 --only-indexes
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_0 --only-indexes
--- OK with --polar-enable-global-index, global indexes are not repacked since they are on parent table but not partitions
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_0 --only-indexes
--- OK with --polar-enable-global-index
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_0 --only-indexes
--- OK for multi partitions
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_0 --table=repack_gi_schema.prt_p_1 --table=repack_gi_schema.prt_p_2 --only-indexes
--- OK for primary key index
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_p_0_pkey
SELECT count(*) FROM prt_p_0;
-- Scan by index, prove that index is ok after repacking
SELECT * FROM prt WHERE b_text = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '5';
SELECT * FROM prt WHERE c_varchar = '5';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '5';

--
-- 7. Repack index on partitioned partition
--
SELECT count(*) FROM prt_p_def;
--- OK, but global indexes are not repacked since they are on parent table but not partitions
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_def --only-indexes
--- OK with --polar-enable-global-index, global indexes are not repacked since they are on parent table but not partitions
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_def --only-indexes
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
--- OK, but global indexes are not repacked since they are on parent table but not partitions
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --only-indexes
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_def_p_0 --only-indexes
--- OK with --polar-enable-global-index, global indexes are not repacked since they are on parent table but not partitions
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --only-indexes
--- OK
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt_p_def_p_0 --only-indexes
--- OK for multi sub-partition
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --table=repack_gi_schema.prt_p_def_p_1 --table='repack_gi_schema."prt_p_def_P_2"' --only-indexes
--- Error, upper case name should be preserved by '' and ""
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --table=repack_gi_schema.prt_p_def_p_1 --table=repack_gi_schema.prt_p_def_P_2 --only-indexes
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --table=repack_gi_schema.prt_p_def_p_1 --table="repack_gi_schema.prt_p_def_P_2" --only-indexes
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_def_p_0 --table=repack_gi_schema.prt_p_def_p_1 --table=repack_gi_schema."prt_p_def_P_2" --only-indexes
--- OK for primary key index
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_p_def_p_0_pkey
-- Scan by index, prove that index is ok after repacking
SELECT * FROM prt WHERE b_text = '7';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE b_text = '7';
SELECT * FROM prt WHERE c_varchar = '7';
EXPLAIN (COSTS off) SELECT * FROM prt WHERE c_varchar = '7';

SELECT count(*) FROM prt_p_def;

--
-- 9. Repack table with unique global index
--
\d+ prt
--- Drop normal global index
DROP INDEX prt_gi_btree_b;
--- Create unique global index
CREATE UNIQUE INDEX prt_gi_btree_b ON prt USING btree (b_text) GLOBAL;
\d+ prt
--- ERROR: cannot repack table with unique index
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_0
--- OK to repack the unique index because it relies on CIC-swap-DI and there are always unique constraint on the table
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_gi_btree_b


--- Create unique global index as primary key
DO $$
BEGIN
    CREATE UNIQUE INDEX prt_pkey_global ON prt(a_int) GLOBAL;
    --- Drop local index primary key
    ALTER TABLE prt DROP CONSTRAINT prt_pkey;
    --- Add global index primary key
    ALTER TABLE prt ADD CONSTRAINT prt_pkey PRIMARY KEY USING INDEX prt_pkey_global;
END $$;
--- Global index is primary key index now
\d+ prt
--- ERROR: cannot repack table with global index primary key
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_gi_schema.prt
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_gi_schema.prt_p_0
--- OK to repack the unique index because it relies on CIC-swap-DI and there are always unique constraint on the table
\! pg_repack --polar-enable-global-index -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_pkey


RESET search_path;
