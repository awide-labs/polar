--
-- no superuser check
--
SET client_min_messages = error;
DROP ROLE IF EXISTS nosuper;
SET client_min_messages = warning;
CREATE ROLE nosuper WITH LOGIN;
-- => OK
\! pg_repack -T 3600 -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_cluster --no-superuser-check
-- => ERROR
\! pg_repack -T 3600 --dbname=contrib_regression_pg_repack --table=tbl_cluster --username=nosuper
-- => ERROR
\! pg_repack -T 3600 --dbname=contrib_regression_pg_repack --table=tbl_cluster --username=nosuper --no-superuser-check
DROP ROLE IF EXISTS nosuper;
