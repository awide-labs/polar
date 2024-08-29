SET client_min_messages = warning;
--
-- create superuser.
--
CREATE USER polar_repack_superuser WITH superuser;
DO $$
BEGIN
	EXECUTE 'GRANT CREATE ON DATABASE ' || current_database() || ' TO polar_repack_superuser';
	GRANT CREATE ON SCHEMA public TO polar_repack_superuser;
END $$;
SET ROLE polar_repack_superuser;
CREATE EXTENSION pg_repack;
CREATE EXTENSION IF NOT EXISTS btree_gin;
CREATE EXTENSION IF NOT EXISTS btree_gist;
RESET client_min_messages;

\dx pg_repack
-- We need this extension for statistics test. No need to create it because it's created during initdb.
-- Do not use `\dx polar_feature_utils` here because it shows the version number and leads to a extra
-- maintenance cost when polar_feature_utils version number is changed.
SELECT 1 AS EXISTS FROM pg_catalog.pg_extension WHERE extname = 'polar_feature_utils';
