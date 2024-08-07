
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS dblink;

DROP TABLE t1;
DROP TABLE t2;
SELECT walminer_stop();
CREATE TABLE t1(i int, j int, k varchar);
CREATE TABLE t2(i int, j int, k varchar);

SELECT 1 FROM pg_switch_wal();

INSERT INTO t1 SELECT generate_series(1,10), 1, 'walminer_test_string';
CHECKPOINT;
-- 测试同一个事务内的多个SQL解析
BEGIN;
INSERT INTO t1 VALUES(1,2,'sql insert');
DELETE FROM t1 WHERE i = 1;
UPDATE t1 SET k = k || ' update' WHERE i = 2;
COMMIT;
CHECKPOINT;
--测试子事务解析
BEGIN;
INSERT INTO t1 VALUES(1,3, 'test subtransaction insert');
INSERT INTO t1 VALUES(2,3, 'test subtransaction insert');
SAVEPOINT s1;
INSERT INTO t1 VALUES(1,4, 'test subtransaction insert');
INSERT INTO t1 VALUES(2,4, 'test subtransaction insert');
UPDATE t1 SET k = k  || ' Movead' WHERE j = 4;
SAVEPOINT s2;
INSERT INTO t2 VALUES(1,1, 'test subtransaction insert');
INSERT INTO t2 VALUES(2,1, 'test subtransaction insert');
INSERT INTO t1 VALUES(1,5, 'test subtransaction insert');
DELETE FROM t2 WHERE i = 1;
SAVEPOINT s3;
INSERT INTO t1 VALUES(1,6, 'test subtransaction insert');
INSERT INTO t2 VALUES(1,2, 'test subtransaction insert');
SAVEPOINT s4;
DELETE FROM t1 WHERE j = 4;
INSERT INTO t1 VALUES(1,7, 'test subtransaction insert');
INSERT INTO t2 VALUES(1,3, 'test subtransaction insert');
ROLLBACK TO SAVEPOINT  s4;
SAVEPOINT s5;
INSERT INTO t1 VALUES(1,8, 'test subtransaction insert');
INSERT INTO t2 VALUES(1,4, 'test subtransaction insert');
COMMIT;
CHECKPOINT;
--测试并行事务
SELECT dblink_connect('dbname='|| current_database() ||' port='|| (select setting from pg_settings where name='port') ||' user=' || current_user) ;
BEGIN;
INSERT INTO t1 VALUES(1,9, 'test concurrence transaction xid 1');
INSERT INTO t2 VALUES(1,5, 'test concurrence transaction xid 1');
SELECT dblink_exec('BEGIN');
SELECT dblink_exec($$ INSERT INTO t1 VALUES(1, 10, 'test concurrence transaction xid 2') $$);
SELECT dblink_exec($$ INSERT INTO t2 VALUES(1, 6, 'test concurrence transaction xid 2') $$);
INSERT INTO t1 VALUES(2,9, 'test concurrence transaction xid 1');
INSERT INTO t2 VALUES(2,5, 'test concurrence transaction xid 1');
COMMIT;
SELECT dblink_exec($$ INSERT INTO t1 VALUES(2, 10, 'test concurrence transaction 2') $$);
SELECT dblink_exec($$ INSERT INTO t2 VALUES(2, 6, 'test concurrence transaction 2') $$);
SELECT dblink_exec($$ COMMIT $$);
CHECKPOINT;

--测试muti insert
COPY t1 (i,j,k) FROM stdin;
1	11	movead
2	11	ashnah
3	11	abc
4	11	def
\.

COPY t2 (i,j,k) FROM stdin;
1	7	movead
2	7	ashnah
3	7	abc
4	7	def
\.


--测试vacuum
DELETE FROM t2 WHERE i = 2;
INSERT INTO t2 VALUES(1,7, 'test_vacuum');
SELECT ctid, * FROM t2 WHERE i = 1 AND j = 7;
VACUUM t2;
INSERT INTO t2 VALUES(1, 8, 'test_vacuum');
INSERT INTO t2 VALUES(1, 9, 'test_vacuum');
SELECT ctid, * FROM t2 WHERE i = 1 AND j > 7;

CHECKPOINT;

SELECT setting  AS pgdata  FROM pg_settings WHERE name = 'data_directory' \gset

SELECT :'pgdata' || '/pg_walminer/wm_datadict/dictionary.d' AS path \gset

SELECT walminer_build_dictionary(:'path');
SELECT walminer_load_dictionary(:'path');

SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset

SELECT :'pgdata' || '/pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_wal_remove(:'walfile_path');
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_regression_mode();
SELECT walminer_all();
\x
SELECT sqlno, topxid=0 as istopxid, op_text, undo_text FROM walminer_contents;
\x


