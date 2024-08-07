/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wm_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WM_UTILS_H
#define WM_UTILS_H 1

#include "postgres.h"
#include "walminer_decode.h"
#include "wm_compatible.h"

extern bool debug_mode;

#define     WALMINER_INT32_MAX      2147483647
#define     WALMINER_INT32_MIN      (-WALMINER_INT32_MAX -1)
#define     WALMINER_VERSION_NUM    "3.0.0"

#define PATH_KIND_INVALID 	0
#define PATH_KIND_NULL		1
#define PATH_KIND_DIR 		2
#define PATH_KIND_SFILE		3
#define PATH_KIND_FILE 		4

bool is_file_exist(char *filepath);
bool is_dir_exist(char *dirpath);
void check_all(void);
int path_judge(char *path);
bool is_path_write_access(char *dirpath);
void create_file(char* path);

char* walminer_malloc(int size,int checkflag);
void walminer_free(char* ptr,int checkflag);
uint64 proCheckBit(char *outPtr, int outsize);
void cheCheckBit(char *outPtr, int outsize, uint64 checkbit);
void walminer_elog(const char *fmt,...) __attribute__((format(gnu_printf,1,2)));
void walminer_debug(const char *fmt,...) __attribute__((format(gnu_printf,1,2)));
char* get_dict_path(char *dict_path);
char* get_runtemp_path(char *runtemp_path);
char* get_image_path(char *image_path);
char* get_log_pach(char *log_path);
char* get_failure_pach(char *failure_path);
char* get_failure_temp_pach(char *failure_path_temp);
char* get_transaction_pach(char *log_path, char* filename);
bool is_empt_str(char *str);
int scan_dir_get_filenum(char *scdir);
int scan_dir_get_filename(char *scdir,NameData *datafilename, bool sigfile);
void drop_allfile_in_dir(char *dir);
#ifndef PG_VERSION_10
int get_wal_seg_size(char* path);
#endif
void split_path_fname(const char *path, char **dir, char **fname);
void trim_space(char *str);

XLogRecPtr prepare_read(bool issearch, XLogRecPtr point_end_lsn);
XLogRecord* get_next_record(XLogReaderState *reader ,void *private_temp ,XLogRecPtr first_record);

ReorderBufferChange *get_change_space(void);

void record_store_image(XLogReaderState *record);
bool get_image_from_store(RelFileNode *rnode, ForkNumber forknum, BlockNumber blkno, char* page, int *index);
ReorderBufferTupleBuf *get_tuple_space(Size tuple_len);
void page_init_by_xlog(RelFileNode *rnode, ForkNumber forknum, BlockNumber blkno);
void get_pageno_range(BlockNumber *maxno, BlockNumber *minno, bool *inited);

FILE* prepare_logfile(void);
FILE* prepare_debugfile(void);
void end_logfile(void);
void end_debug_file(void);

void fix_path_end(char *path);
bool number_in_array(int *array, int len, int num);

void walminer_init_failure_file(void);
void walminer_init_failure_temp_file(void);

#endif