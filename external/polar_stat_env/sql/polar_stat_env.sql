--
-- Test cases for polar_stat_env.
--

CREATE EXTENSION IF NOT EXISTS polar_stat_env;

SELECT position('CPU' IN s) != 0 FROM polar_stat_env() s;
SELECT position('CPU' IN s) != 0 FROM polar_stat_env_no_format() s;

SELECT position('CPU' IN s) != 0 FROM polar_stat_env('json') s;
SELECT position('CPU' IN s) != 0 FROM polar_stat_env_no_format('json') s;

SELECT position('CPU' IN s) != 0 FROM polar_stat_env('text') s;
SELECT position('CPU' IN s) != 0 FROM polar_stat_env_no_format('text') s;

SELECT position('CPU' IN s) != 0 FROM polar_stat_env('yaml') s;
SELECT position('CPU' IN s) != 0 FROM polar_stat_env_no_format('yaml') s;

DROP EXTENSION polar_stat_env;
