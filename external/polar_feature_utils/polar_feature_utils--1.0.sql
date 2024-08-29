-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_feature_utils" to load this file. \quit

-- The view cannot be created in pg_catalog
CREATE SCHEMA IF NOT EXISTS polar_feature_utils;

CREATE OR REPLACE FUNCTION polar_feature_utils.polar_advisor_feature_name(IN i INT4)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE STRICT;

CREATE OR REPLACE FUNCTION polar_feature_utils.polar_advisor_feature_value(IN i INT4)
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE STRICT;

CREATE OR REPLACE FUNCTION polar_feature_utils.polar_advisor_feature_count()
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE STRICT;

-- UniverseExplorer relies on this view to monitor the feature usage
CREATE OR REPLACE VIEW polar_feature_utils.polar_unique_feature_usage AS
    SELECT
        i AS id,
        polar_feature_utils.polar_advisor_feature_name(i) AS name,
        polar_feature_utils.polar_advisor_feature_value(i) AS value
    FROM generate_series(0, polar_feature_utils.polar_advisor_feature_count() - 1) i;

-- We should open this view to normal user even if the extension is invisible to user because:
-- 1. We have introduced this view to public on Mo Tian Lun (https://www.modb.pro/).
-- 2. The regression test use polar-superuser and it doesn't have the privilege to access this view.
GRANT USAGE ON SCHEMA polar_feature_utils TO PUBLIC;
GRANT SELECT ON polar_feature_utils.polar_unique_feature_usage TO PUBLIC;
