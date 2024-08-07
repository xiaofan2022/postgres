
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;
DROP TABLE t1;
DROP TABLE t2;

SELECT 1 FROM pg_switch_wal();
CHECKPOINT;
CREATE TABLE t1(i int PRIMARY KEY, j int, k varchar);
CREATE TABLE t2(i int, j int, k varchar);
SELECT oid AS target_oid_t1 FROM pg_class WHERE relname ='t1' \gset
SELECT oid AS target_oid_t2 FROM pg_class WHERE relname ='t2' \gset

INSERT INTO t1 VALUES(1,1,'walminer');
INSERT INTO t2 VALUES(1,1,'walminer');
INSERT INTO t2 VALUES(2,2,'walminer');
INSERT INTO t1 VALUES(2,2,'walminer');
DELETE FROM t1 WHERE i = 1;
DELETE FROM t2 WHERE i = 1;
UPDATE t1 SET k = k || ' PostgreSQL' WHERE i = 2;
UPDATE t2 SET k = k || ' PostgreSQL' WHERE i = 2;
VACUUM t1;
VACUUM t2;
INSERT INTO t1 VALUES(3,3,'walminer');
INSERT INTO t2 VALUES(3,3,'walminer');
INSERT INTO t2 VALUES(4,4,'walminer');
INSERT INTO t1 VALUES(4,4,'walminer');
DELETE FROM t1 WHERE i = 3;
DELETE FROM t2 WHERE i = 3;
UPDATE t1 SET k = k || ' PostgreSQL' WHERE i = 4;
UPDATE t2 SET k = k || ' PostgreSQL' WHERE i = 4;

-- 预测插入测试
INSERT INTO t1 VALUES(5,1,'test ON CONFLICT') ON CONFLICT(i) DO NOTHING;
INSERT INTO t1 VALUES(2,1,'test ON CONFLICT DO NOTHING') ON CONFLICT(i) DO NOTHING;
INSERT INTO t1 VALUES(2,1,'test ON CONFLICT DO UPDATE') ON CONFLICT(i)
    DO UPDATE SET i = (SELECT max(i)+1 FROM t1), k = EXCLUDED.k;

-- 2PC测试
BEGIN;
INSERT INTO t1 VALUES(9,1,'test 2PC commit');
INSERT INTO t2 VALUES(9,2,'test 2PC commit');
PREPARE TRANSACTION 'pt1';
BEGIN;
INSERT INTO t1 VALUES(7,1,'test 2PC rollback');
INSERT INTO t2 VALUES(7,2,'test 2PC rollback');
PREPARE TRANSACTION 'pt2';

INSERT INTO t1 VALUES(8,1,'test 2PC show');
INSERT INTO t2 VALUES(8,2,'test 2PC show');
ROLLBACK PREPARED 'pt2';
COMMIT PREPARED 'pt1';



SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_regression_mode();
SELECT wal2sql(:target_oid_t1::oid);
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

SELECT walminer_stop();
SELECT walminer_debug_mode();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_regression_mode();
SELECT wal2sql(:target_oid_t2::oid);
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

SELECT walminer_stop();
SELECT walminer_debug_mode();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_regression_mode();
SELECT wal2sql();
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;