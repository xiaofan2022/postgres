
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;

DROP TABLE t1;
DROP TABLE t2;
SELECT walminer_stop();
CREATE TABLE t1(i int, j int, k varchar);
CREATE TABLE t2(i int, j int, k varchar);
SELECT relfilenode AS node FROM pg_class WHERE relname ='t1' \gset

SELECT 1 FROM pg_switch_wal();
CHECKPOINT;
INSERT INTO t1 VALUES(0,0, 'lsn_test');
CHECKPOINT;
CHECKPOINT;
INSERT INTO t1 VALUES(1,1, 'lsn_test');
INSERT INTO t2 VALUES(2,1, 'lsn_test');
SELECT pg_current_wal_lsn() AS lsn1 \gset
INSERT INTO t1 VALUES(1,2, 'lsn_test');
INSERT INTO t2 VALUES(2,2, 'lsn_test');
SELECT pg_current_wal_lsn() AS lsn2 \gset
INSERT INTO t1 VALUES(1,3, 'lsn_test');
INSERT INTO t2 VALUES(2,3, 'lsn_test');

VACUUM FULL t1;

CREATE TABLE t11(i int, j int, k varchar);
SELECT 1 FROM walminer_table_avatar('t11', :node);

SELECT walminer_regression_mode();
SELECT walminer_by_lsn(:'lsn1', :'lsn2');
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;
SELECT walminer_stop();

SELECT 1 FROM walminer_table_avatar('t11', :node);
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_regression_mode();
SELECT wal2sql();
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;