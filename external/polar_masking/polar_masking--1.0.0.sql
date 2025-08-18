-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_masking" to load this file. \quit

CREATE SCHEMA if NOT EXISTS polar_masking; 

CREATE TABLE polar_masking.polar_masking_label_tab(labelid INT, relid OID);
CREATE UNIQUE INDEX polar_masking_label_tab_relid_idx ON polar_masking.polar_masking_label_tab (relid);

CREATE TABLE polar_masking.polar_masking_label_col(labelid INT, relid OID, colid SMALLINT);
CREATE UNIQUE INDEX polar_masking_label_col_relid_colid_idx ON polar_masking.polar_masking_label_col (relid, colid);

CREATE TABLE polar_masking.polar_masking_policy(labelid INT, name TEXT, operator SMALLINT);
CREATE UNIQUE INDEX polar_masking_policy_labelid_idx ON polar_masking.polar_masking_policy (labelid);

CREATE SEQUENCE polar_masking.polar_masking_labelid_sequence
    START WITH 1
    INCREMENT BY 1
    MINVALUE 1
    MAXVALUE 2147483647
    NO CYCLE;

CREATE FUNCTION polar_masking.polar_masking_create_label(label_name text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_create_label';

CREATE FUNCTION polar_masking.polar_masking_drop_label(label_name text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_drop_label';

CREATE FUNCTION polar_masking.polar_masking_apply_label_to_table(label_name text, shema_name text, table_name text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_apply_label_to_table';

CREATE FUNCTION polar_masking.polar_masking_remove_table_from_label(label_name text, shema_name text, table_name text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_remove_table_from_label';

CREATE FUNCTION polar_masking.polar_masking_apply_label_to_column(label_name text, shema_name text, table_name text, column_name text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_apply_label_to_column';
REVOKE ALL ON FUNCTION polar_masking.polar_masking_apply_label_to_column FROM PUBLIC;

CREATE FUNCTION polar_masking.polar_masking_remove_column_from_label(label_name text, shema_name text, table_name text, column_name text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_remove_column_from_label';
REVOKE ALL ON FUNCTION polar_masking.polar_masking_remove_column_from_label FROM PUBLIC;

CREATE FUNCTION polar_masking.polar_masking_alter_label_maskingop(label_name text, maskingop text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_alter_label_maskingop';
REVOKE ALL ON FUNCTION polar_masking.polar_masking_alter_label_maskingop FROM PUBLIC;
