\echo Use "CREATE EXTENSION polar_password_policy" to load this file. \quit

CREATE SCHEMA IF NOT EXISTS polar_security;

CREATE FUNCTION polar_security.polar_create_password_policy (
	policy_name name
) RETURNS void
AS 'MODULE_PATHNAME', 'polar_create_password_policy'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION polar_security.polar_create_password_policy (
	policy_name name,
	password_complexity integer,
	password_min_length integer,
	password_max_length integer,
	password_valid_time integer,
	password_reuse_count integer,
	password_reuse_time integer,
	password_fail_count integer,
	password_lock_time integer
) RETURNS void
AS 'MODULE_PATHNAME', 'polar_create_password_policy_option'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION polar_security.polar_alter_password_policy (
	policy_name name,
	option_name name,
	option_value integer
) RETURNS void
AS 'MODULE_PATHNAME', 'polar_alter_password_policy'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION polar_security.polar_rename_password_policy (
	policy_name name,
	new_policy_name name
) RETURNS void
AS 'MODULE_PATHNAME', 'polar_rename_password_policy'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION polar_security.polar_drop_password_policy (
	policy_name name
) RETURNS void
AS 'MODULE_PATHNAME', 'polar_drop_password_policy'
LANGUAGE C STABLE STRICT;

CREATE TABLE polar_security.polar_password_policy (
	policy_oid OID,
	policy_name name,
	password_complexity integer,
	password_min_length integer,
	password_max_length integer,
	password_valid_time integer,
	password_reuse_count integer,
	password_reuse_time integer,
	password_fail_count integer,
	password_lock_time integer
);
CREATE UNIQUE INDEX polar_password_policy_oid_index ON polar_security.polar_password_policy (policy_oid);
CREATE UNIQUE INDEX polar_password_policy_name_index ON polar_security.polar_password_policy (policy_name);

SELECT polar_security.polar_create_password_policy('default_password_policy');
