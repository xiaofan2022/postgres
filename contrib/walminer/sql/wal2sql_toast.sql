
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;
DROP TABLE t1;

SELECT 1 FROM pg_switch_wal();
CHECKPOINT;

CREATE TABLE t1(i int, j int, k varchar);
ALTER TABLE t1 ALTER COLUMN k SET STORAGE EXTERNAL;

SELECT 'pg_toast.pg_toast_' || oid AS toast_table_name FROM pg_class WHERE relname ='t1' \gset

INSERT INTO t1 VALUES(1,1,repeat('PostgreSQL',2800));
UPDATE t1 SET k = k || 'test_toast' WHERE i = 1;
DELETE FROM t1 WHERE i = 1;

select chunk_id,chunk_seq from :toast_table_name;

SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_regression_mode();
SELECT wal2sql();
SELECT sqlno, topxid=0 as istopxid, length(op_text) FROM walminer_contents;