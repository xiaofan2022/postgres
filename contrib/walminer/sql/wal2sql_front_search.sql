
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;

DROP TABLE t1;
DROP TABLE t2;

CREATE TABLE t1(i int, j int, k varchar);
CREATE TABLE t2(i int, j int, k varchar);

SELECT 1 FROM pg_switch_wal();
INSERT INTO t1 VALUES(0,0, 'front_search');
SELECT pg_current_wal_lsn() AS lsn0 \gset

INSERT INTO t1 VALUES(0,0, 'front_search');
CHECKPOINT;
INSERT INTO t1 VALUES(1,0, 'front_search');
SELECT pg_current_wal_lsn() AS lsn1 \gset
BEGIN;
INSERT INTO t1 VALUES(1,1, 'front_search');
INSERT INTO t1 VALUES(2,1, 'front_search');
COMMIT;
INSERT INTO t2 VALUES(1,0, 'front_search');
INSERT INTO t2 VALUES(2,0, 'front_search');

SELECT pg_current_wal_lsn() AS lsn2 \gset

SELECT walminer_regression_mode();

SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT wal2sql(:'lsn0'::pg_lsn, :'lsn2'::pg_lsn,'false');
SELECT sqlno, topxid=0 as istopxid, sqlkind, minerd, op_text,complete FROM walminer_contents;

SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_lsn(:'lsn0'::pg_lsn, :'lsn2'::pg_lsn, 'true');

SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_lsn(:'lsn1'::pg_lsn, :'lsn2'::pg_lsn,'true');
SELECT sqlno, topxid=0 as istopxid, sqlkind, minerd, op_text,complete FROM walminer_contents;