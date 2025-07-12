CREATE EXTENSION polar_password_policy;

\d+ polar_security.polar_password_policy
SELECT
	policy_name,
	password_complexity,
	password_min_length,
	password_max_length,
	password_valid_time,
	password_reuse_count,
	password_reuse_time,
	password_fail_count,
	password_lock_time
FROM polar_security.polar_password_policy;

\d+ polar_security.polar_password_policy_binding

-- create
SELECT polar_security.polar_create_password_policy('p');
SELECT polar_security.polar_create_password_policy('pp',3,8,64,30,3,5,3,60);
SELECT
	policy_name,
	password_complexity,
	password_min_length,
	password_max_length,
	password_valid_time,
	password_reuse_count,
	password_reuse_time,
	password_fail_count,
	password_lock_time
FROM polar_security.polar_password_policy;

SELECT polar_security.polar_create_password_policy('p');
SELECT polar_security.polar_create_password_policy('ppp',1,8,64,30,3,5,3,60);
SELECT polar_security.polar_create_password_policy('ppp',3,80,64,30,3,5,3,60);

-- alter
SELECT polar_security.polar_alter_password_policy('p','password_valid_time',10);
SELECT polar_security.polar_alter_password_policy('default_password_policy','password_complexity',3);
SELECT
	policy_name,
	password_complexity,
	password_min_length,
	password_max_length,
	password_valid_time,
	password_reuse_count,
	password_reuse_time,
	password_fail_count,
	password_lock_time
FROM polar_security.polar_password_policy;

SELECT polar_security.polar_alter_password_policy('ppp','password_valid_time',10);
SELECT polar_security.polar_alter_password_policy('p','password_valid_ti',10);
SELECT polar_security.polar_alter_password_policy('p','password_valid_time',360);
SELECT polar_security.polar_alter_password_policy('p','password_min_length',80);

-- rename
SELECT polar_security.polar_rename_password_policy('p','p1');
SELECT
	policy_name,
	password_complexity,
	password_min_length,
	password_max_length,
	password_valid_time,
	password_reuse_count,
	password_reuse_time,
	password_fail_count,
	password_lock_time
FROM polar_security.polar_password_policy;

SELECT polar_security.polar_rename_password_policy('default_password_policy','p');
SELECT polar_security.polar_rename_password_policy('pp','default_password_policy');
SELECT polar_security.polar_rename_password_policy('pp','p1');
SELECT polar_security.polar_rename_password_policy('ppp','p');

-- drop
SELECT polar_security.polar_drop_password_policy('pp');
SELECT
	policy_name,
	password_complexity,
	password_min_length,
	password_max_length,
	password_valid_time,
	password_reuse_count,
	password_reuse_time,
	password_fail_count,
	password_lock_time
FROM polar_security.polar_password_policy;

SELECT polar_security.polar_drop_password_policy('default_password_policy');
SELECT polar_security.polar_drop_password_policy('p');

-- bind
CREATE ROLE role1;
SELECT policy_name FROM polar_security.polar_password_policy
	WHERE policy_oid = (
		SELECT policy_oid FROM polar_security.polar_password_policy_binding
			WHERE role_oid = (
				SELECT oid FROM pg_authid WHERE rolname = 'role1'
			)
	);
SELECT polar_security.polar_bind_password_policy('role1','p1');
SELECT policy_name FROM polar_security.polar_password_policy
	WHERE policy_oid = (
		SELECT policy_oid FROM polar_security.polar_password_policy_binding
			WHERE role_oid = (
				SELECT oid FROM pg_authid WHERE rolname = 'role1'
			)
	);

SELECT polar_security.polar_bind_password_policy('role2','p1');
SELECT polar_security.polar_bind_password_policy('role1','p2');
SELECT polar_security.polar_drop_password_policy('p1');

-- unbind
SELECT polar_security.polar_unbind_password_policy('role1');
SELECT policy_name FROM polar_security.polar_password_policy
	WHERE policy_oid = (
		SELECT policy_oid FROM polar_security.polar_password_policy_binding
			WHERE role_oid = (
				SELECT oid FROM pg_authid WHERE rolname = 'role1'
			)
	);
DROP ROLE role1;
SELECT * FROM polar_security.polar_password_policy_binding;
SELECT polar_security.polar_drop_password_policy('p1');

SELECT polar_security.polar_unbind_password_policy('role1');
