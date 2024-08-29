-- This regression test case is used for coverage testing.
-- The feature index bulk extend test is in test/polar_pl.
-- Index bulk extend is used in big table index insert, 
-- the regression test cases don't create an big table.
ALTER SYSTEM SET polar_index_bulk_extend_size = 512;
ALTER SYSTEM SET polar_min_bulk_extend_table_size = 0;
SELECT pg_reload_conf();
SELECT pg_sleep(2);
show polar_index_bulk_extend_size;
show polar_min_bulk_extend_table_size;
CREATE TABLE test_index_bulk_extend(test1 int, test2 int);
CREATE INDEX test_index_bulk on test_index_bulk_extend(test1);
INSERT INTO test_index_bulk_extend values(generate_series(1, 10000), generate_series(1, 10000));
SELECT * FROM test_index_bulk_extend ORDER BY test1 limit 10;
DROP INDEX test_index_bulk;
DROP TABLE test_index_bulk_extend;
ALTER SYSTEM RESET polar_index_bulk_extend_size;
ALTER SYSTEM RESET polar_min_bulk_extend_table_size;
SELECT pg_reload_conf();