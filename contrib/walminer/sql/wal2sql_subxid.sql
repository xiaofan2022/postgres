DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;

DROP TABLE t1;
SELECT walminer_stop();
CREATE TABLE t1(i int, j int, k varchar);

SELECT pg_current_wal_lsn() AS lsn1 \gset

BEGIN;
INSERT INTO t1 VALUES(1,1, 'test subtransaction insert');
SAVEPOINT s1;
INSERT INTO t1 VALUES(2,1, 'test subtransaction insert');
SAVEPOINT s2;
INSERT INTO t1 VALUES(3,1, 'test subtransaction insert');
ROLLBACK TO s2;
SAVEPOINT s3;
INSERT INTO t1 VALUES(4,1, 'test subtransaction insert');
COMMIT;

BEGIN;
INSERT INTO t1 VALUES(5,1, 'test subtransaction insert');
SAVEPOINT s1;
INSERT INTO t1 VALUES(6,1, 'test subtransaction insert');
ROLLBACK TO s1;
INSERT INTO t1 VALUES(7,1, 'test subtransaction insert');
COMMIT;

SELECT pg_current_wal_lsn() AS lsn2 \gset
SELECT walminer_regression_mode();
SELECT wal2sql(:'lsn1'::pg_lsn, :'lsn2'::pg_lsn, 'true');
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;