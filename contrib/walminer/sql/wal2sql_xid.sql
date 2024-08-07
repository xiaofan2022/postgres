
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
INSERT INTO t1 VALUES(1,1, 'time_xid');
SELECT txid_current() AS xid1 \gset
COMMIT;

BEGIN;
INSERT INTO t1 VALUES(1,2, 'time_xid');
INSERT INTO t2 VALUES(2,2, 'time_xid');
SELECT txid_current() AS xid2 \gset
END;

SELECT txid_current() AS xid3 \gset
INSERT INTO t1 VALUES(1,3, 'time_xid');
INSERT INTO t2 VALUES(2,3, 'time_xid');
SELECT txid_current() AS xid4 \gset

SELECT walminer_regression_mode();

SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT wal2sql(:'xid1'::int);
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;


SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_xid(:'xid2');
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_xid(:'xid3'::int + 1);
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;