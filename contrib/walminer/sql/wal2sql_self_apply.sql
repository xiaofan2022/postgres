
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;

DROP TABLE t1;
DROP TABLE t2;
DROP TABLE t3;
SELECT walminer_stop();
CREATE TABLE t1(i int, j int, k varchar);
CREATE TABLE t2(i int primary key, j int, k varchar);
SELECT 1 FROM pg_switch_wal();
CHECKPOINT;
SELECT pg_current_wal_lsn() AS lsn0 \gset
INSERT INTO t1 SELECT generate_series(1,4), 0, 'self_apply_test';
INSERT INTO t2 SELECT generate_series(1,4), 0, 'self_apply_test';
-- 测试系统表不受影响
CREATE TABLE t3(i int, j int, k varchar);
BEGIN;
INSERT INTO t1 VALUES(10,1,'insert t1');
UPDATE t1 SET j = j + 1 WHERE i = 1;
DELETE FROM t1 WHERE i = 2;
UPDATE t1 SET j = j + 1 where i = 10;
COMMIT;

BEGIN;
INSERT INTO t2 VALUES(9, 1,'insert t2 1');
INSERT INTO t2 VALUES(10, 1,'insert t2 1');
INSERT INTO t2 VALUES(11, 1,'insert t2 1');
COMMIT;

BEGIN;
INSERT INTO t2 VALUES(12, 1,'insert t2 2');
INSERT INTO t2 VALUES(13, 1,'insert t2 2');
COMMIT;
SELECT pg_current_wal_lsn() AS lsn1 \gset

DELETE FROM t1;

/* 最后t2表中会遗留10数据，因此这个事务应该apply失败
 * 9 10 11应该不存在于结果中
 */
DELETE FROM t2 WHERE i <> 10;
/*DELETE FROM t2;*/

SELECT walminer_stop();
SELECT walminer_regression_mode();
SELECT walminer_debug_mode();
SELECT walminer_apply(:'lsn0', :'lsn1');

/*
 * postgres=# select * from t1;
 * i  | j |        k        
 * ----+---+-----------------
 *  3 | 0 | self_apply_test
 *  4 | 0 | self_apply_test
 *  1 | 1 | self_apply_test
 * 10 | 2 | insert t1
 * (4 rows)
 */
SELECT * FROM t1;

/*
 * postgres=# select * from t2;
 * i  | j |        k        
 * ----+---+-----------------
 * 10 | 1 | insert t2 1
 * 1 | 0 | self_apply_test
 * 2 | 0 | self_apply_test
 * 3 | 0 | self_apply_test
 * 4 | 0 | self_apply_test
 * 12 | 1 | insert t2 2
 * 13 | 1 | insert t2 2
 * (6 rows)
 */
SELECT * FROM t2;
