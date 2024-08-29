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
DROP EXTENSION test_polar_rsc;
