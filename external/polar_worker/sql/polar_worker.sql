-- Test access privileges
--
--setup
create extension polar_worker;

SELECT test_read_core_pattern();

--cleanup
drop extension polar_worker;
