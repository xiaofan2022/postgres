/* walminer--3.0.sql */

CREATE OR REPLACE FUNCTION wal2sql_internal(starttime text, endtime text, startlsn pg_lsn, endlsn pg_lsn,
										xid int, fsearch bool, reloid Oid, tempresult bool)
RETURNS TEXT AS
'MODULE_PATHNAME','wal2sql_internal'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION page_collect(relfilenode Oid, reloid Oid, pagenos CSTRING DEFAULT 'all',
										diff_report bool DEFAULT 'true')
RETURNS TEXT AS
'MODULE_PATHNAME','page_collect_internal'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION pg_walusage()
RETURNS TEXT AS
'MODULE_PATHNAME','pg_walusage'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION walminer_mrecords_inmemory(records int)
RETURNS INT AS
'MODULE_PATHNAME','walminer_mrecords_inmemory'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION wal2sql_self_apply()
RETURNS INT AS
'MODULE_PATHNAME','wal2sql_self_apply'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION walminer_regression_mode()
RETURNS BOOL AS
'MODULE_PATHNAME','walminer_regression_mode'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION walminer_debug_mode()
RETURNS BOOL AS
'MODULE_PATHNAME','walminer_debug_mode'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION wal2sql_with_catalog()
RETURNS BOOL AS
'MODULE_PATHNAME','wal2sql_with_catalog'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION wal2sql_with_ddl()
RETURNS BOOL AS
'MODULE_PATHNAME','wal2sql_with_ddl'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION walminer_help()
RETURNS CSTRING AS
'MODULE_PATHNAME','walminer_help'
LANGUAGE C CALLED ON NULL INPUT;


CREATE OR REPLACE FUNCTION walminer_build_dictionary(in path CSTRING)
RETURNS CSTRING AS
'MODULE_PATHNAME','walminer_build_dictionary'
LANGUAGE C CALLED ON NULL INPUT;


CREATE OR REPLACE FUNCTION walminer_load_dictionary(in path CSTRING)
RETURNS CSTRING AS
'MODULE_PATHNAME','walminer_load_dictionary'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION walminer_stop()
RETURNS CSTRING AS
'MODULE_PATHNAME','walminer_stop'
LANGUAGE C VOLATILE STRICT;

CREATE OR REPLACE FUNCTION walminer_wal_add(in path CSTRING)
RETURNS CSTRING AS
'MODULE_PATHNAME','walminer_wal_add'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION walminer_wal_remove(in path CSTRING)
RETURNS CSTRING AS
'MODULE_PATHNAME','walminer_wal_remove'
LANGUAGE C CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION walminer_wal_list()
RETURNS setof record  AS
'MODULE_PATHNAME','walminer_wal_list'
LANGUAGE C VOLATILE STRICT;

CREATE OR REPLACE FUNCTION walminer_table_avatar(tablename CSTRING, relfilenode Oid)
RETURNS CSTRING  AS
'MODULE_PATHNAME','walminer_table_avatar'
LANGUAGE C VOLATILE STRICT;

CREATE OR REPLACE FUNCTION walminer_version()
RETURNS CSTRING AS 
'MODULE_PATHNAME','walminer_version'
LANGUAGE C VOLATILE STRICT;



CREATE OR REPLACE FUNCTION walminer_contents_check()
RETURNS void AS
$BODY$
DECLARE
	rd "varchar";
	checksql "varchar";
	temptablename "varchar";
	tp "varchar";
BEGIN
	temptablename := 'walminer_contents';
	tp :='u';
	SELECT * into rd FROM pg_catalog.pg_class WHERE relname = 'walminer_contents' AND relpersistence = 'u';
	IF FOUND THEN
		TRUNCATE TABLE walminer_contents;
	ELSE
		CREATE UNLOGGED TABLE walminer_contents(sqlno int, xid bigint, topxid bigint,sqlkind int, minerd bool,
									timestamp timestampTz, op_text text, undo_text text, complete bool,
									schema text, relation text, start_lsn pg_lsn, commit_lsn pg_lsn);
	END IF;
END;
$BODY$
LANGUAGE 'plpgsql' VOLATILE;

CREATE OR REPLACE FUNCTION walminer_by_time(starttime text, endtime text, fsearch bool DEFAULT 'false',
											reloid Oid DEFAULT 0, tempresult bool DEFAULT 'false')
RETURNS TEXT AS 
$BODY$
	select walminer_contents_check();
	select wal2sql_internal($1, $2, NULL, NULL, 0, $3, $4, $5);
$BODY$
LANGUAGE 'sql';

CREATE OR REPLACE FUNCTION wal2sql(starttime text, endtime text, fsearch bool DEFAULT 'false',
											reloid Oid DEFAULT 0, tempresult bool DEFAULT 'false')
RETURNS TEXT AS 
$BODY$
	select walminer_by_time($1, $2, $3, $4, $5);
$BODY$
LANGUAGE 'sql';


CREATE OR REPLACE FUNCTION walminer_by_lsn(startlsn pg_lsn, endlsn pg_lsn, fsearch bool DEFAULT 'false',
											reloid Oid DEFAULT 0, tempresult bool DEFAULT 'false')
RETURNS TEXT AS 
$BODY$
	select walminer_contents_check();
	select wal2sql_internal(NULL, NULL, $1, $2 , 0, $3, $4, $5);
$BODY$
LANGUAGE 'sql';

CREATE OR REPLACE FUNCTION wal2sql(startlsn pg_lsn, endlsn pg_lsn, fsearch bool DEFAULT 'false',
											reloid Oid DEFAULT 0, tempresult bool DEFAULT 'false')
RETURNS TEXT AS 
$BODY$
	select walminer_by_lsn($1, $2, $3, $4, $5);
$BODY$
LANGUAGE 'sql';

CREATE OR REPLACE FUNCTION walminer_apply(startlsn pg_lsn, endlsn pg_lsn, fsearch bool DEFAULT 'true',
											reloid Oid DEFAULT 0, tempresult bool DEFAULT 'false')
RETURNS TEXT AS 
$BODY$
	select wal2sql_self_apply();
	select walminer_by_lsn($1, $2, $3, $4, $5);
$BODY$
LANGUAGE 'sql';

CREATE OR REPLACE FUNCTION walminer_by_xid(xid int, fsearch bool DEFAULT 'false',
											reloid Oid DEFAULT 0, tempresult bool DEFAULT 'false')
RETURNS TEXT AS 
$BODY$
	select walminer_contents_check();
	select wal2sql_internal(NULL, NULL, NULL, NULL, $1, $2, $3, $4);
$BODY$
LANGUAGE 'sql';

CREATE OR REPLACE FUNCTION wal2sql(xid int, fsearch bool DEFAULT 'false',
											reloid Oid DEFAULT 0, tempresult bool DEFAULT 'false')
RETURNS TEXT AS 
$BODY$
	select walminer_by_xid($1, $2, $3, $4);
$BODY$
LANGUAGE 'sql';

CREATE OR REPLACE FUNCTION walminer_all(reloid Oid DEFAULT 0, tempresult bool DEFAULT 'false')
RETURNS TEXT AS 
$BODY$
	select walminer_contents_check();
	select wal2sql_internal(NULL, NULL, NULL, NULL, 0, 'false', $1, $2);
$BODY$
LANGUAGE 'sql';

CREATE OR REPLACE FUNCTION wal2sql(reloid Oid DEFAULT 0, tempresult bool DEFAULT 'false')
RETURNS TEXT AS 
$BODY$
	select walminer_all($1, $2);
$BODY$
LANGUAGE 'sql';


