--
-- Test polar_force_unlogged_to_logged_table
--

SET polar_force_unlogged_to_logged_table TO ON;
-- Should be forced to a logged table
CREATE UNLOGGED TABLE tstu (
	i	int4,
	t	text
);
-- Should success and do nothing
ALTER TABLE tstu SET LOGGED;
DROP TABLE tstu;
-- Should be forced to a logged table
SELECT generate_series(1,10) INTO UNLOGGED TABLE tstu;
DROP TABLE tstu;
CREATE TABLE tstu (
	i	int4,
	t	text
);
-- Should be forbidden
ALTER TABLE tstu SET UNLOGGED;
DROP TABLE tstu;

SET polar_force_unlogged_to_logged_table TO OFF;
-- Should be an unlogged table
CREATE UNLOGGED TABLE tstu (
	i	int4,
	t	text
);
-- Should success
ALTER TABLE tstu SET LOGGED;
DROP TABLE tstu;
CREATE TABLE tstu (
	i	int4,
	t	text
);
-- Should success
ALTER TABLE tstu SET UNLOGGED;
DROP TABLE tstu;
