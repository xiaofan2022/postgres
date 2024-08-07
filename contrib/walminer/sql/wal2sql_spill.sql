
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
INSERT INTO t1 SELECT generate_series(1,50), 1, 'walminer_test_spill';
COMMIT;

SELECT walminer_stop();

SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT walminer_regression_mode();
SELECT walminer_mrecords_inmemory(11);
SELECT wal2sql();
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

SELECT walminer_mrecords_inmemory(20);
SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT wal2sql();
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

SELECT walminer_mrecords_inmemory(49);
SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT wal2sql();
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

SELECT walminer_mrecords_inmemory(50);
SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT wal2sql();
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;

SELECT walminer_mrecords_inmemory(51);
SELECT walminer_stop();
SELECT pg_walfile_name(pg_current_wal_lsn()) AS walfile_name \gset
SELECT 'pg_wal/' || :'walfile_name' AS walfile_path \gset
SELECT walminer_wal_add(:'walfile_path');
SELECT wal2sql();
SELECT sqlno, topxid=0 as istopxid, op_text FROM walminer_contents;