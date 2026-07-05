CREATE EXTENSION polar_monitor;
CREATE EXTENSION test_polar_shm_aset;

SELECT test_shm_aset_random(3, 5, 1024, 4096, 'random');
SELECT test_shm_aset_random(3, 5, 1024, 4096, 'forwards');
SELECT test_shm_aset_random(3, 5, 1024, 4096, 'backwards');

SELECT count(*) from test_shm_aset_random_parallel(3, 5, 1024, 8192, 'random', 5);
SELECT count(*) from test_shm_aset_random_parallel(3, 5, 1024, 8192, 'forwards', 5);
SELECT count(*) from test_shm_aset_random_parallel(3, 5, 1024, 8192, 'backwards', 5);

SELECT * FROM polar_shm_aset_ctl;
SELECT * FROM polar_stat_dsa;