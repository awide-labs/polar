CREATE EXTENSION test_polar_rsc;

-- create table
CREATE TABLE test_rsc (id int, txt text);

-- cache not loaded
SELECT in_cache, main_cache
    FROM test_polar_rsc_stat_entries()
    WHERE rel_node = (SELECT relfilenode FROM pg_class WHERE relname = 'test_rsc');

-- cache loaded
SELECT * FROM test_rsc;
SELECT in_cache, main_cache
    FROM test_polar_rsc_stat_entries()
    WHERE rel_node = (SELECT relfilenode FROM pg_class WHERE relname = 'test_rsc');

-- cache updated due to extension
INSERT INTO test_rsc VALUES (1, 'cause page extend');
SELECT in_cache, main_cache
    FROM test_polar_rsc_stat_entries()
    WHERE rel_node = (SELECT relfilenode FROM pg_class WHERE relname = 'test_rsc');

-- cache hit by searching ref
SELECT * FROM test_rsc;
SELECT test_polar_rsc_search_by_ref(relfilenode)
    FROM pg_class WHERE relname = 'test_rsc';

-- cache hit by searching mapping
SELECT test_polar_rsc_search_by_mapping(relfilenode)
    FROM pg_class WHERE relname = 'test_rsc';

-- cache invalidation
TRUNCATE test_rsc;
SELECT in_cache, main_cache
    FROM test_polar_rsc_stat_entries()
    WHERE rel_node = (SELECT relfilenode FROM pg_class WHERE relname = 'test_rsc') AND in_cache = true;

-- cache miss by searching ref
SELECT test_polar_rsc_search_by_ref(relfilenode)
    FROM pg_class WHERE relname = 'test_rsc';

-- cache miss by searching mapping
SELECT test_polar_rsc_search_by_mapping(relfilenode)
    FROM pg_class WHERE relname = 'test_rsc';

-- reload cache
SELECT test_polar_rsc_update_entry(relfilenode)
    FROM pg_class WHERE relname = 'test_rsc';
SELECT in_cache, main_cache
    FROM test_polar_rsc_stat_entries()
    WHERE rel_node = (SELECT relfilenode FROM pg_class WHERE relname = 'test_rsc') AND in_cache = true;

-- cache hit by searching mapping
SELECT test_polar_rsc_search_by_mapping(relfilenode)
    FROM pg_class WHERE relname = 'test_rsc';

DROP TABLE test_rsc;

-- XCOM-159: with RSC enabled, DROP TABLE must consult RSC for the
-- relation size to take the targeted buffer-invalidation path instead
-- of scanning all of shared_buffers.  Pre-fix, the drop path never
-- called any RSC lookup helper, so the nblocks_*_hit counters stayed
-- at zero across the drop.
CREATE TABLE test_rsc_drop AS
    SELECT g AS id, repeat('x', 200) AS payload
        FROM generate_series(1, 1000) g;
SELECT count(*) FROM test_rsc_drop;
SELECT test_polar_rsc_reset_nblocks_stat();
DROP TABLE test_rsc_drop;
SELECT test_polar_rsc_nblocks_hits() > 0 AS drop_used_rsc;

DROP EXTENSION test_polar_rsc;
