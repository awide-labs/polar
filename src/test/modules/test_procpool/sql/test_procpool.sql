CREATE EXTENSION test_procpool;

-- See README for explanation of arguments:
SELECT test_procpool(1);
SELECT test_procpool(10000);
SELECT test_procpool(100000);
SELECT test_procpool(100000);
SELECT test_procpool(1000000);
SELECT pg_reload_conf();
SELECT test_procpool(1000000);
SELECT test_procpool(100000);
