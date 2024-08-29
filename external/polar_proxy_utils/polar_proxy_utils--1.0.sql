/* external/polar_proxy_utils/polar_proxy_utils--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION polar_proxy_utils" to load this file. \quit

CREATE TABLE polar_proxy_routing_strategy
(
	id serial NOT NULL,
	name VARCHAR(64) NOT NULL,
	type SMALLINT NOT NULL,
	rw_mode SMALLINT NOT NULL,
	last_modified TIMESTAMP NOT NULL DEFAULT current_timestamp
);

COMMENT ON TABLE polar_proxy_routing_strategy IS 'Items in this table will change the routing strategy of POLARDB Read/Write Splitting service.';

COMMENT ON COLUMN polar_proxy_routing_strategy.name IS 'The name of the item.';
COMMENT ON COLUMN polar_proxy_routing_strategy.type IS '0 stands for table name, 1 stands for function name';
COMMENT ON COLUMN polar_proxy_routing_strategy.rw_mode IS '0 stands for writing request, 1 stands for reading request. e.g. If the rw_type of table name pg_proc is specified as 0 which means writing request, then the SELECT request contains pg_proc will be regarded as a writting request.';
COMMENT ON COLUMN polar_proxy_routing_strategy.last_modified IS 'The Greenwich Mean Time of the creation of this item, default is calculated by function now().';

CREATE INDEX polar_proxy_routing_strategy_id ON polar_proxy_routing_strategy (id);
CREATE UNIQUE INDEX polar_proxy_routing_strategy_name_type ON polar_proxy_routing_strategy (name, type);
CREATE INDEX polar_proxy_routing_strategy_last_modified ON polar_proxy_routing_strategy (last_modified);

CREATE OR REPLACE FUNCTION polar_proxy_routing_strategy_modify_timestamp()
RETURNS TRIGGER
AS $$
BEGIN
	NEW.last_modified := current_timestamp;
	RETURN NEW;
END
$$ LANGUAGE plpgsql;

CREATE TRIGGER
	polar_proxy_routing_strategy_modify_timestamp_trigger
BEFORE UPDATE ON
	polar_proxy_routing_strategy
FOR EACH ROW EXECUTE FUNCTION
	polar_proxy_routing_strategy_modify_timestamp();

-- Proxy operation interface

-- Get all routing strategy
CREATE OR REPLACE FUNCTION polar_list_proxy_routing_strategy(
	OUT name	VARCHAR,
	OUT type	VARCHAR,
	OUT rw_mode	VARCHAR,
	OUT last_modified	TIMESTAMP)
RETURNS SETOF record AS
$$
	SELECT
		name,
		CASE type
			WHEN 0 THEN 'table'
			WHEN 1 THEN 'function'
			ELSE 'invalid'
		END,
		CASE rw_mode
			WHEN 0 THEN 'write'
			WHEN 1 THEN 'read'
			ELSE 'invalid'
		END,
		last_modified
	FROM polar_proxy_routing_strategy;
$$ LANGUAGE SQL VOLATILE;

COMMENT ON FUNCTION polar_list_proxy_routing_strategy IS 'List all proxy routing strategy.';

CREATE VIEW polar_proxy_all_routing_strategy(name, type, rw_mode, last_modified) AS
	SELECT
	name,
	CASE type
		WHEN 0 THEN 'table'
		WHEN 1 THEN 'function'
		ELSE 'invalid'
	END,
	CASE rw_mode
		WHEN 0 THEN 'write'
		WHEN 1 THEN 'read'
		ELSE 'invalid'
	END,
	last_modified
	FROM polar_proxy_routing_strategy;

COMMENT ON VIEW polar_proxy_all_routing_strategy IS 'All proxy routing strategy.';


-- Add new routing strategy
CREATE OR REPLACE FUNCTION polar_add_proxy_routing_strategy(
	_name	VARCHAR(64),
	_type	CHARACTER,
	rw_mode	CHARACTER)
RETURNS VOID AS
$$
DECLARE
	__type	SMALLINT;
	rw	SMALLINT;
	num	INTEGER;
BEGIN
	IF _type = 't' THEN
		__type := 0;
	ELSIF _type = 'f' THEN
		__type := 1;
	ELSE
		RAISE EXCEPTION 'type is out of valid range (t for table name, f for function name).';
	END IF;

	IF rw_mode = 'w' THEN
		rw := 0;
	ELSIF rw_mode = 'r' THEN
		rw := 1;
	ELSE
		RAISE EXCEPTION 'rw_mode is out of valid range (w for wite, r for read).';
	END IF;

	EXECUTE 'select count(*) from polar_proxy_routing_strategy;' into num;

	IF num >= 10000 THEN
		RAISE EXCEPTION 'beyond table limit(10000), try to remove some entry.';
	END IF;

	BEGIN
		INSERT INTO
		polar_proxy_routing_strategy(name, type, rw_mode)
		VALUES (_name, __type, rw);
	EXCEPTION
		WHEN unique_violation THEN
			RAISE EXCEPTION 'duplicated name.';
	END;
END
$$ LANGUAGE plpgsql VOLATILE;

COMMENT ON FUNCTION polar_add_proxy_routing_strategy IS 'Add proxy routing strategy, t for table and f for function in type, w for write and r for read in rw_mode.';


-- Delete routing strategy entry
CREATE OR REPLACE FUNCTION polar_delete_proxy_routing_strategy(
	_name VARCHAR,
	_type VARCHAR)
RETURNS VOID AS
$$
DECLARE
	__type	SMALLINT;
BEGIN
	IF _type = 't' THEN
		__type := 0;
	ELSIF _type = 'f' THEN
		__type := 1;
	ELSE
		RAISE EXCEPTION 'type is out of valid range (t for table name, f for function name).';
	END IF;

	DELETE FROM polar_proxy_routing_strategy WHERE name = $1 and type = __type;
END
$$ LANGUAGE plpgsql VOLATILE;

COMMENT ON FUNCTION polar_delete_proxy_routing_strategy IS 'Delete one proxy routing strategy specified by name.';


-- Truncate routing strategy entry
CREATE OR REPLACE FUNCTION polar_truncate_proxy_routing_strategy()
RETURNS VOID AS
$$
	TRUNCATE TABLE  polar_proxy_routing_strategy;
$$ LANGUAGE SQL VOLATILE;

COMMENT ON FUNCTION polar_truncate_proxy_routing_strategy IS 'Delete all proxy routing strategy.';

GRANT SELECT, INSERT, UPDATE, TRUNCATE, DELETE ON TABLE polar_proxy_routing_strategy TO PUBLIC;
GRANT SELECT ON TABLE polar_proxy_all_routing_strategy TO PUBLIC;
GRANT SELECT, UPDATE ON SEQUENCE polar_proxy_routing_strategy_id_seq TO PUBLIC;
