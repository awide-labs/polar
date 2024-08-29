--
-- do repack
--
SET ROLE polar_repack_superuser;

\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_cluster
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_badindex
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --polar-no-disable
