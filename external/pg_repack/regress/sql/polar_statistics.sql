CREATE TEMP TABLE repack_stats_tmp AS SELECT id, name, value FROM polar_feature_utils.polar_unique_feature_usage WHERE name LIKE 'Repack%' ORDER BY id;

-- repack 1 normal table
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_normal.normal
SELECT t.name, u.value - t.value AS increase FROM polar_feature_utils.polar_unique_feature_usage u, repack_stats_tmp t WHERE u.id = t.id;

-- repack 1 partitioned table, including multiple partitions
DELETE FROM repack_stats_tmp;
INSERT INTO repack_stats_tmp SELECT id, name, value FROM polar_feature_utils.polar_unique_feature_usage WHERE name LIKE 'Repack%' ORDER BY id;
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --parent-table=repack_part_schema.prt
SELECT t.name, u.value - t.value AS increase FROM polar_feature_utils.polar_unique_feature_usage u, repack_stats_tmp t WHERE u.id = t.id;

-- repack 1 normal index
DELETE FROM repack_stats_tmp;
INSERT INTO repack_stats_tmp SELECT id, name, value FROM polar_feature_utils.polar_unique_feature_usage WHERE name LIKE 'Repack%' ORDER BY id;
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_normal.normal_pkey
SELECT t.name, u.value - t.value AS increase FROM polar_feature_utils.polar_unique_feature_usage u, repack_stats_tmp t WHERE u.id = t.id;

-- repack multiple indexes
DELETE FROM repack_stats_tmp;
INSERT INTO repack_stats_tmp SELECT id, name, value FROM polar_feature_utils.polar_unique_feature_usage WHERE name LIKE 'Repack%' ORDER BY id;
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_normal.normal_pkey --index=repack_normal.normal_idx_btree_a --index=repack_normal.normal_idx_gin_b --index=repack_normal.normal_idx_gist_c
SELECT t.name, u.value - t.value AS increase FROM polar_feature_utils.polar_unique_feature_usage u, repack_stats_tmp t WHERE u.id = t.id;

-- repack all indexes of table
DELETE FROM repack_stats_tmp;
INSERT INTO repack_stats_tmp SELECT id, name, value FROM polar_feature_utils.polar_unique_feature_usage WHERE name LIKE 'Repack%' ORDER BY id;
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=repack_normal.normal --only-indexes
SELECT t.name, u.value - t.value AS increase FROM polar_feature_utils.polar_unique_feature_usage u, repack_stats_tmp t WHERE u.id = t.id;

-- repack global index
DELETE FROM repack_stats_tmp;
INSERT INTO repack_stats_tmp SELECT id, name, value FROM polar_feature_utils.polar_unique_feature_usage WHERE name LIKE 'Repack%' ORDER BY id;
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --index=repack_gi_schema.prt_gi_btree_b --polar-enable-global-index
SELECT t.name, u.value - t.value AS increase FROM polar_feature_utils.polar_unique_feature_usage u, repack_stats_tmp t WHERE u.id = t.id;

-- RepackApplyLogCount never increase because there's no concurrent DML during repacking. We cover it in test framework
