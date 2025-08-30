-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_masking" to load this file. \quit

CREATE SCHEMA if NOT EXISTS polar_masking; 

CREATE TABLE polar_masking.polar_masking_label_tab(labelid INT, relid OID);
CREATE UNIQUE INDEX polar_masking_label_tab_relid_idx ON polar_masking.polar_masking_label_tab (relid);

CREATE TABLE polar_masking.polar_masking_label_col(labelid INT, relid OID, colid SMALLINT);
CREATE UNIQUE INDEX polar_masking_label_col_relid_colid_idx ON polar_masking.polar_masking_label_col (relid, colid);

CREATE TABLE polar_masking.polar_masking_policy(labelid INT, name TEXT, operator SMALLINT);
CREATE UNIQUE INDEX polar_masking_policy_labelid_idx ON polar_masking.polar_masking_policy (labelid);

create table polar_masking.polar_masking_policy_regex(labelid INT, start_pos Integer, end_pos Integer, regex text, replace_text text);
CREATE UNIQUE INDEX polar_masking_policy_regex_labelid_idx ON polar_masking.polar_masking_policy_regex (labelid);

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

CREATE FUNCTION polar_masking.polar_masking_remove_column_from_label(label_name text, shema_name text, table_name text, column_name text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_remove_column_from_label';

CREATE FUNCTION polar_masking.polar_masking_alter_label_maskingop(label_name text, maskingop text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_alter_label_maskingop';

CREATE FUNCTION polar_masking.polar_masking_alter_label_maskingop_set_regexpmasking(label_name text, start_pos Integer, end_pos Integer, regex text, replace_text text) RETURNS VOID
VOLATILE LANGUAGE C AS 'MODULE_PATHNAME', 'polar_masking_alter_label_maskingop_set_regexpmasking';

CREATE OR REPLACE FUNCTION polar_masking.creditcardmasking(col text, letter char default 'x') RETURNS text AS $$
declare 
    size INTEGER := 4;
begin
    return CASE WHEN pg_catalog.length(col) >= size THEN
        pg_catalog.REGEXP_REPLACE(pg_catalog.left(col, size*(-1)), '[\d+]', letter, 'g') || pg_catalog.right(col, size)
    ELSE
        col
    end;
end;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION polar_masking.basicemailmasking(col text, letter char default 'x') RETURNS text AS $$
declare 
    pos INTEGER := position('@' in col);
begin
    return CASE WHEN pos > 1 THEN
    pg_catalog.repeat(letter, pos - 1) || pg_catalog.substring(col, pos, pg_catalog.length(col) - pos +1)
    ELSE
        col
    end;
end;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION polar_masking.fullemailmasking(col text, letter char default 'x') RETURNS text AS $$
declare 
    pos INTEGER := position('@' in col);
    dot_pos INTEGER := pg_catalog.length(col) - position('.' in pg_catalog.reverse(col)) + 1;
    d_pos INTEGER := position('.' in pg_catalog.reverse(col));
begin
    return CASE WHEN pos > 0 and dot_pos > pos and d_pos > 0 THEN
    pg_catalog.repeat(letter, pos - 1) || '@' || pg_catalog.repeat(letter,  dot_pos - pos - 1) || pg_catalog.substring(col, dot_pos, pg_catalog.length(col) - dot_pos +1)
    WHEN pos = 0 and d_pos > 0 THEN
    pg_catalog.repeat(letter,  dot_pos - 1) || pg_catalog.substring(col, dot_pos, pg_catalog.length(col) - dot_pos +1)
    ELSE
        col
    end;
end;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION polar_masking.alldigitsmasking(col text, letter char default '0') RETURNS text AS $$
begin
    return pg_catalog.REGEXP_REPLACE(col, '[\d+]', letter, 'g');
end;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION polar_masking.shufflemasking(col text) RETURNS text AS $$
declare 
    rd INTEGER;
    size INTEGER := pg_catalog.length(col);
    tmp text := col;
    res text := '';
begin
    while size > 0 loop 
        rd := pg_catalog.floor(pg_catalog.random() * pg_catalog.length(tmp) + 1);
        res := res || pg_catalog.right(pg_catalog.left(tmp, rd), 1);
        tmp := pg_catalog.left(tmp, rd - 1) || pg_catalog.right(tmp, pg_catalog.length(tmp) - rd);
        size := size - 1;
    END loop;
    return res;
end;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION polar_masking.randommasking(col text) RETURNS text AS $$	
begin
    return pg_catalog.left(pg_catalog.MD5(pg_catalog.random()::text), pg_catalog.length(col));
end;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION polar_masking.maskall(col text, letter char default 'x') RETURNS text AS $$
declare 
    size INTEGER := pg_catalog.length(col);
begin
    return pg_catalog.repeat(letter, size);
end;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION polar_masking.regexpmasking(col text, reg text, replace_text text, start_pos INTEGER default 0, end_pos INTEGER default 0) RETURNS text AS $$
declare
	size INTEGER := pg_catalog.length(col);
	endpos INTEGER := end_pos;
	startpos INTEGER := start_pos;
	lstr text;
	rstr text;
	ltarget text;
begin
	IF startpos <= 0 THEN startpos := 1; END IF;
	IF startpos > size THEN startpos := size + 1; END IF;
	IF endpos <= 0 THEN endpos := size; END IF;
	IF endpos > size THEN endpos := size; END IF;
    lstr := pg_catalog.left(col, startpos - 1);
    rstr := pg_catalog.right(col, size - endpos);
    ltarget := pg_catalog.substring(col, startpos, endpos - startpos+ 1);
    ltarget := pg_catalog.regexp_replace(ltarget, reg, replace_text, 'g');
    return lstr || ltarget || rstr;
end;
$$ LANGUAGE plpgsql;
