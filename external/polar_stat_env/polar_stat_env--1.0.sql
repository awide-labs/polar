-- Create customized polar stat env func
CREATE FUNCTION polar_stat_env(
    IN format text DEFAULT 'json'
)
RETURNS SETOF TEXT
AS 'MODULE_PATHNAME', 'polar_stat_env'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION polar_stat_env_no_format(
    IN format text DEFAULT 'json'
)
RETURNS SETOF TEXT
AS 'MODULE_PATHNAME', 'polar_stat_env_no_format'
LANGUAGE C PARALLEL SAFE;
