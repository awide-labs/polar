--
-- do repack
--

\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_cluster --error-on-invalid-index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --table=tbl_badindex --error-on-invalid-index
\! pg_repack -T 3600 -k -U polar_repack_superuser --dbname=contrib_regression_pg_repack --error-on-invalid-index --polar-no-disable
