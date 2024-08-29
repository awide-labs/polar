CREATE SCHEMA polar_sql_mapping;
GRANT USAGE ON SCHEMA polar_sql_mapping to PUBLIC;

--sql mapping table can't create rowid, otherwise all attnum will add one.
SELECT pg_catalog.set_config(name,'0','f')
FROM pg_catalog.pg_settings
WHERE name in ('default_with_oids','polar_default_with_rowid');

CREATE TABLE polar_sql_mapping.polar_sql_mapping_table
(
       id         BIGINT GENERATED ALWAYS AS IDENTITY (SEQUENCE NAME polar_sql_mapping.polar_sql_mapping_id_sequences) PRIMARY KEY,
       source_sql TEXT   COLLATE "C" NOT NULL,
       target_sql TEXT   COLLATE "C" NOT NULL
);

CREATE UNIQUE INDEX polar_sql_mapping_source_sql_unique_idx ON polar_sql_mapping.polar_sql_mapping_table(source_sql);

CREATE FUNCTION polar_sql_mapping.insert_mapping(sourcesql TEXT, targetsql TEXT)
RETURNS VOID
LANGUAGE plpgsql AS $$
BEGIN
       INSERT INTO 
       polar_sql_mapping.polar_sql_mapping_table(source_sql,target_sql)
       VALUES(sourcesql,targetsql);

       RETURN;
END
$$
VOLATILE STRICT;


/* Now define */
CREATE FUNCTION polar_sql_mapping.error_sql_info(
    OUT id bigint,
    OUT query text,
    OUT emessage text,
    OUT calls int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'error_sql_info'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW polar_sql_mapping.error_sql_info AS
  SELECT id,
         query COLLATE "C",
         emessage COLLATE "C",
         calls
  FROM polar_sql_mapping.error_sql_info();

CREATE FUNCTION polar_sql_mapping.error_sql_info_clear()
RETURNS void
AS 'MODULE_PATHNAME', 'error_sql_info_clear'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION polar_sql_mapping.insert_mapping_id(error_id BIGINT, targetsql TEXT)
RETURNS VOID
LANGUAGE plpgsql AS $$
DECLARE
       sourcesql TEXT;
BEGIN
       SELECT query INTO STRICT sourcesql FROM 
       polar_sql_mapping.error_sql_info 
       WHERE polar_sql_mapping.error_sql_info.id = error_id;

       PERFORM polar_sql_mapping.insert_mapping(sourcesql, targetsql);

       RETURN; 

       EXCEPTION
       WHEN NO_DATA_FOUND THEN
              RAISE WARNING 'id % not found', error_id;
              RETURN;
       WHEN TOO_MANY_ROWS THEN
              RAISE DEBUG 'id % not unique', error_id;
              RETURN;
END
$$
VOLATILE STRICT;

--Grant access to general users
GRANT SELECT
ON SEQUENCE polar_sql_mapping.polar_sql_mapping_id_sequences
TO PUBLIC;

GRANT SELECT
ON TABLE polar_sql_mapping.polar_sql_mapping_table
TO PUBLIC;

GRANT SELECT ON polar_sql_mapping.error_sql_info TO PUBLIC;

GRANT EXECUTE
ON ALL FUNCTIONS IN SCHEMA polar_sql_mapping
TO PUBLIC;
