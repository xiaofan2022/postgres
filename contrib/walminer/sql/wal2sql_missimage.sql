
DROP EXTENSION IF EXISTS walminer;
CREATE EXTENSION IF NOT EXISTS walminer;

DROP TABLE t1;
DROP TABLE t2;

CREATE TABLE t1(i int, j int, k varchar);
CREATE TABLE t2(i int, j int, k varchar);

INSERT INTO t1 VALUES(0,0, 'miss_image');
INSERT INTO t1 VALUES(1,0, 'miss_image');

SELECT 1 FROM pg_switch_wal();
BEGIN;
INSERT INTO t1 VALUES(1,1, 'miss_image');
UPDATE t1 SET k = k || ' Movead' WHERE i = 1;
DELETE FROM t1 WHERE i = 0;
COMMIT;

SELECT walminer_regression_mode();

SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_all();
SELECT sqlno, topxid=0 as istopxid, sqlkind, minerd, op_text FROM walminer_contents;