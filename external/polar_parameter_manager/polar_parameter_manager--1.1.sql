\echo Use "CREATE EXTENSION polar_parameter_manager" to load this file. \quit
CREATE SCHEMA polar_parameter;

CREATE FUNCTION polar_parameter.polar_get_guc_info(name text, 
OUT is_visible BOOLEAN, OUT is_user_changable BOOLEAN, OUT optional text, OUT is_list BOOLEAN)
RETURNS SETOF record
AS '$libdir/polar_parameter_manager', 'polar_get_guc_info'
LANGUAGE C STABLE STRICT
PARALLEL SAFE;

CREATE OR REPLACE VIEW polar_parameter.parameter_infos_memory AS
SELECT    a.name::character varying               AS name,
          a.boot_val::character varying           AS default_value,
     CASE WHEN c.setting IS NOT NULL THEN c.setting::character varying  
          ELSE a.setting::character varying  
     END                                          AS setting_value,
     CASE WHEN a.context = 'postmaster' THEN 0
          ELSE 1
     END                                          AS is_dynamic,
     CASE WHEN c.sourcefile IS NOT NULL AND c.sourcefile like '%postgresql.conf' THEN 0
          ELSE b.is_visible::int 
     END                                          AS is_visible,
     CASE WHEN c.sourcefile IS NOT NULL AND c.sourcefile like '%postgresql.conf' THEN 0
          ELSE b.is_user_changable::int 
     END                                          AS is_user_changable,
     CASE WHEN b.optional IS NOT NULL THEN b.optional
          WHEN a.vartype = 'bool' THEN '[on|off]'
          WHEN a.enumvals IS NOT NULL THEN '[' || array_to_string(enumvals,'|') || ']'
          WHEN a.max_val::numeric >= '999999999999'::numeric AND a.vartype = 'real'
               THEN '[' || a.min_val || '-999999999999.0]'
          WHEN a.min_val IS NOT NULL OR a.max_val IS NOT NULL 
               THEN '[' || a.min_val || '-' || a.max_val || ']'
          ELSE '^.*$'
     END                                          AS optional,
     CASE WHEN a.vartype = 'integer'  THEN 'INT'
          WHEN a.vartype = 'real' THEN 'DOUBLE' 
          ELSE 'STRING'
     END                                          AS unit,
     CASE WHEN a.vartype = 'integer' OR a.vartype = 'real' THEN 1
          ELSE 0
     END                                          AS divide_base,
          a.short_desc::character varying         AS comment,
     CASE WHEN c.sourcefile IS NOT NULL AND c.sourcefile not like '%polar_settings.conf'
               THEN 'setting is in postgresql.(auto).conf'::character varying
          ELSE NULL
     END                                          AS unchangable_reason,
          b.is_list::int                          AS is_list
FROM pg_settings a LEFT JOIN 
(SELECT name, setting, sourcefile FROM pg_file_settings where applied = 't' ) c
ON lower(a.name) = c.name, polar_parameter.polar_get_guc_info(a.name) b;

-- Only used to force paramaers infos
CREATE TABLE polar_parameter.parameter_infos_force (LIKE polar_parameter.parameter_infos_memory);
CREATE UNIQUE INDEX ON polar_parameter.parameter_infos_force(name);

CREATE OR REPLACE VIEW polar_parameter.parameter_infos
AS
SELECT a.name,
COALESCE(b.default_value, a.default_value) as default_value,
COALESCE(b.setting_value, a.setting_value) as setting_value,
COALESCE(b.is_dynamic, a.is_dynamic) as is_dynamic,
COALESCE(b.is_visible, a.is_visible) as is_visible,
COALESCE(b.is_user_changable, a.is_user_changable) as is_user_changable,
COALESCE(b.optional, a.optional) as optional,
COALESCE(b.unit, a.unit) as unit,
COALESCE(b.divide_base, a.divide_base) as divide_base,
COALESCE(b.comment, a.comment) as comment,
COALESCE(b.unchangable_reason, a.unchangable_reason) as unchangable_reason,
COALESCE(b.is_list, a.is_list) as is_list
FROM polar_parameter.parameter_infos_memory a
LEFT JOIN polar_parameter.parameter_infos_force b
ON a.name = b.name;

-- Record the parameter values when the instance was first created, so that the default_value is fixed.
INSERT INTO polar_parameter.parameter_infos_force(name, default_value)
     SELECT name, setting_value
     FROM polar_parameter.parameter_infos_memory;
