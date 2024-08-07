
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;

DROP TABLE t1;
DROP TABLE t2;
SELECT walminer_stop();
CREATE TABLE t1(i int, j int, k varchar);
CREATE TABLE t2(i int, j int, k varchar);

SELECT 1 FROM pg_switch_wal();
CHECKPOINT;
BEGIN;
INSERT INTO t1 VALUES(1,1, 'time_test');
INSERT INTO t2 VALUES(2,1, 'time_test');
COMMIT;

SELECT now() AS time1 \gset

INSERT INTO t1 VALUES(1,2, 'time_test');
INSERT INTO t2 VALUES(2,2, 'time_test');

SELECT now() AS time2 \gset

BEGIN;
INSERT INTO t1 VALUES(1,3, 'time_test');
INSERT INTO t2 VALUES(2,3, 'time_test');
COMMIT;

SELECT now() AS time3 \gset

BEGIN;
INSERT INTO t1 VALUES(1,4, 'time_test');
INSERT INTO t2 VALUES(2,4, 'time_test');
COMMIT;

SELECT walminer_regression_mode();

SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT wal2sql(:'time1'::text, :'time2'::text);
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;


SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_time(:'time2'::text, :'time3'::text);
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_time(:'time1'::text, :'time3'::text);
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;