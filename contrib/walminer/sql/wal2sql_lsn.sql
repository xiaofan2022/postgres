
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;

DROP TABLE t1;
DROP TABLE t2;
SELECT walminer_stop();
CREATE TABLE t1(i int, j int, k varchar, primary key(i,j));
CREATE TABLE t2(i int, j int, k varchar, primary key(j,k));
CREATE TABLE t3(i int, j int, k varchar, primary key(i,j,k));

SELECT 1 FROM pg_switch_wal();
CHECKPOINT;
INSERT INTO t1 VALUES(0,0, 'lsn_test');
CHECKPOINT;
CHECKPOINT;
INSERT INTO t1 VALUES(1,1, 'lsn_test');
INSERT INTO t2 VALUES(2,1, 'lsn_test');
INSERT INTO t3 VALUES(3,1, 'lsn_test');
SELECT pg_current_wal_lsn() AS lsn1 \gset
INSERT INTO t1 VALUES(1,2, 'lsn_test');
INSERT INTO t2 VALUES(2,2, 'lsn_test');
INSERT INTO t3 VALUES(3,2, 'lsn_test');
UPDATE t1 SET k = 'lsn_test1' WHERE j = 2;
UPDATE t2 SET k = 'lsn_test1' WHERE j = 2;
UPDATE t3 SET k = 'lsn_test1' WHERE j = 2;
DELETE FROM t1 WHERE j = 2;
DELETE FROM t2 WHERE j = 2;
DELETE FROM t3 WHERE j = 2;
SELECT pg_current_wal_lsn() AS lsn2 \gset
INSERT INTO t1 VALUES(1,3, 'lsn_test');
INSERT INTO t2 VALUES(2,3, 'lsn_test');
INSERT INTO t3 VALUES(3,3, 'lsn_test');
DELETE FROM t1 WHERE j = 3;
DELETE FROM t2 WHERE j = 3;
DELETE FROM t3 WHERE j = 3;
SELECT pg_current_wal_lsn() AS lsn3 \gset
INSERT INTO t1 VALUES(1,4, 'lsn_test');
INSERT INTO t2 VALUES(2,4, 'lsn_test');
INSERT INTO t3 VALUES(3,4, 'lsn_test');
UPDATE t1 SET k = 'lsn_test1', i = 10, j = 40 WHERE i = 1 AND j = 4;
UPDATE t2 SET k = 'lsn_test1', i = 10, j = 40 WHERE i = 2 AND j = 4;
UPDATE t3 SET k = 'lsn_test1', i = 10, j = 40 WHERE i = 2 AND j = 4;

SELECT walminer_regression_mode();
--SELECT walminer_debug_mode();

SELECT walminer_stop();
-- SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
-- SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
-- SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_lsn(:'lsn1', :'lsn2', 'true');
SELECT schema, relation, sqlno, topxid=0 as istopxid, op_text,
            start_lsn >= :'lsn1' and start_lsn <= :'lsn2',
            commit_lsn >= :'lsn1' and commit_lsn <= :'lsn2'
FROM walminer_contents;


SELECT walminer_stop();
-- SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
-- SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
-- SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_lsn(:'lsn2', :'lsn3', 'true');
SELECT schema, relation, sqlno, topxid=0 as istopxid, op_text,
            start_lsn >= :'lsn2' and start_lsn <= :'lsn3',
            commit_lsn >= :'lsn2' and commit_lsn <= :'lsn3'
FROM walminer_contents;

SELECT walminer_stop();
-- SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
-- SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
-- SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_lsn(:'lsn1', :'lsn3', 'true');
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

SELECT walminer_stop();
-- SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
-- SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
-- SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_by_lsn(:'lsn2', NULL, 'true');
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;