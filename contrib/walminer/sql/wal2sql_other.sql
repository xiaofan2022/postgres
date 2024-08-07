
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;

DROP TABLE t1;
SELECT walminer_stop();
CHECKPOINT;
SELECT pg_current_wal_lsn() AS lsn1 \gset
CREATE TABLE t1(i INT, j INT, k VARCHAR);
INSERT INTO t1 VALUES(1,1,NULL);
UPDATE t1 SET i = 10 WHERE i = 1;
DELETE FROM t1 WHERE i = 10;
SELECT pg_current_wal_lsn() AS lsn2 \gset
SELECT walminer_regression_mode();
SELECT wal2sql(:'lsn1'::pg_lsn, :'lsn2'::pg_lsn, 'true');
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

