
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;
CHECKPOINT;
DROP TABLE t1;
DROP TABLE t2;

SELECT walminer_stop();
SELECT pg_current_wal_lsn() AS lsn1 \gset

CREATE TABLE t1(i int, j int, k varchar);
CREATE TABLE t2(i int, j text, k varchar);
TRUNCATE t1;
ALTER TABLE t2 ADD COLUMN m int;
ALTER TABLE t2 DROP COLUMN k; 
ALTER TABLE t2 ALTER COLUMN m TYPE int8;
ALTER TABLE t2 RENAME COLUMN m to n;
ALTER TABLE t2 RENAME TO t2_1;

DROP TABLE t1;
SELECT pg_current_wal_lsn() AS lsn2 \gset

SELECT wal2sql_with_ddl();
SELECT walminer_regression_mode();
SELECT wal2sql(:'lsn1'::pg_lsn, :'lsn2'::pg_lsn, 'true');
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;
