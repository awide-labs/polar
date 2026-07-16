CREATE EXTENSION test_polar_datamax;

-- The argument is the reference WAL image checked in next to this test;
-- pg_regress exports the module source directory as PG_ABS_SRCDIR.
\getenv abs_srcdir PG_ABS_SRCDIR
SELECT test_polar_datamax(:'abs_srcdir' || '/000000010000000000000001');
