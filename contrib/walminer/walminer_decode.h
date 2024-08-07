/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  walminer_decode.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALMINER_DECODE_H
#define WALMINER_DECODE_H

#include "datadictionary.h"
#include "walminer_contents.h"
#include "access/xact.h"
#include "wm_compatible.h"

#define	DECODE_SQL_KIND_INSERT				1
#define	DECODE_SQL_KIND_UPDATE				2
#define	DECODE_SQL_KIND_DELETE				3
#define	DECODE_SQL_KIND_DDL					4
#define	DECODE_SQL_KIND_MUTIINSERT			5
#define	DECODE_SQL_KIND_BEGIN				6
#define	DECODE_SQL_KIND_COMMIT				7
#define	DECODE_SQL_KIND_ABORT				8


#define	MAX_CHANGES_IN_MEMORY  4096


/*便利性临时结构*/
typedef struct WalRecordBuffer
{
	XLogRecPtr	origptr;
	XLogRecPtr	endptr;
	XLogReaderState *record;
} WalRecordBuffer;

typedef void (*walminer_decode_commit_func)(WalRecordBuffer *buf, xl_xact_parsed_commit *parsed, TransactionId xid);
typedef void* (*walminer_search_wal_func) (void* temp);
typedef bool (*walminer_wait_search_func)(void);
typedef bool (*walminer_check_statue_func)(void);
typedef bool (*walminer_filter_in_decode_func)(RelFileNode *target_node, Oid reloid);
typedef void (*walminer_front_read_func)(void);
typedef void (*walminer_wait_thread_func)(void);
typedef void (*walminer_set_pages_func)(void);
typedef void (*walminer_handle_argument_func)(PG_FUNCTION_ARGS);

typedef struct WalminerCallbk{
	walminer_search_wal_func			walminer_search_wal;
	walminer_wait_search_func			walminer_wait_search;
	walminer_check_statue_func			walminer_check_statue;
	walminer_decode_commit_func			walminer_decode_commit;
	walminer_front_read_func			walminer_front_read;
	walminer_filter_in_decode_func		walminer_filter_in_decode;
	walminer_wait_thread_func			walminer_wait_thread;
	walminer_set_pages_func				walminer_set_pages;
	walminer_handle_argument_func		walminer_handle_argument;
}WalminerCallbk;

/* 一个事务的入口 */
/*typedef struct ReorderBufferTXNByIdEnt
{
	TransactionId xid;
	ReorderBufferTXN *txn;
} ReorderBufferTXNByIdEnt;*/

typedef struct SQLRecord
{
	bool				init;
	StringInfoData		sql_pro;					/* SQL暂存 */
	StringInfoData		sql_undo;					/* undo暂存 */
	StringInfoData		notsql;						/* 非SQL暂存 */
	bool				issql;						/* 是否为sql */
	bool				isddl;						/* 是否为DDL */
	bool				get_undo;					/* 是否获取了undo */

	StringInfoData		sql_temp_1;					/* insert的att组，update的新value*/
	StringInfoData		sql_temp_2;					/* insert的value组， update的旧value*/		
}SQLRecord;

typedef struct WalminerPrivate
{
	TimeLineID	timeline;
	char	   *inpath;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	bool		endptr_reached;

	int			sendFile;
	XLogSegNo 	sendSegNo;
	uint32		sendOff;
	ListCell	*cell;
	int			wal_seg_sz;

	bool		search;
} WalminerPrivate;

/* 记录一个事务的所有信息 */
typedef struct TransactionEntry
{
	TransactionId 	xid;						/* 当前事务ID */
	bool			complete;					/* 当前事务确信是完整的 */
	
	/* 读取wal日志过程记录的信息 */
	bool			find_start;					/* 是否找到事务开始的标志(当前是否为完整的事务) */
	bool			has_catalog_changes;		/* 事务内是否有系统表更改信息 */
	XLogRecPtr		first_lsn;					/* 添加这个Rntry的record的起点LSN */
	XLogRecPtr		final_lsn;					/* commit或abort这个事务的record的开始位置 */
	XLogRecPtr		end_lsn;					/* commit或abort这个事务的record的结束位置+1 */
	int32			nentries;					/* 事务内记录的record的数量 */
	int32			nentries_mem;				/* 事务内记录的已经在磁盘上的record的数量 */
	bool			disked;						/* 事务太大导致落盘 */
	bool			finish_indraft;				/* 完成changes读取 */
	FILE			*fp;
	List			*changes;					/* 初步decoding的changes列表 */
	TimestampTz 	commit_time;				/* 事务结束时间 */
	RepOriginId 	origin_id;
	XLogRecPtr 		origin_lsn;
	bool			issubtransaction;			/* 是否为子事务，在遇到父事务commit时才会赋值 */
	TransactionId 	topxid;						/* 父事务事务ID */


	/* 解析过程的记录的信息 */
	List			*toast_list;				/* toast 暂存 */
	List			*analysesql;				/* DDL解析支持结构TODO(lchch) */

	SQLRecord		sql;						/* TODO(lchch)这个结构可以放到更外层
													的结构里，用以减少空间申请 */
	int				sqlno;

	/* 在self_apply功能中记录相关信息 */
	/* 在self_apply功能中，一个事务中只要有一个命令失败，整个事务要丢弃，
		对丢弃的事务要存储于一个记录文件中，由于我们不知道这个事务最后会
		不会成功apply，因此对所有的命令预先记录在temp文件中，事务成功则
		清空temp文件，否则将temp里的内容复制到记录文件中。
	*/
	bool			alive;						/* 在这个事务中是否已经发生回滚，如果发生了回滚
													那么就不需要尝试做apply，只需要写入记录文件即可 */

	/* 表数据解析支持结构 */
	Oid				reloid;						/* 当前解析record所属表的oid */
	Oid				nspoid;						/* 当前解析record所属表的模式的oid */
	NameData		namespace;					/* 当前解析record所属表的模式 */
	NameData		relname;					/* 当前解析record所属表的表名 */
	int				relkind;					/* 当前解析record所属表的类型 */
	int				sqlkind;					/* update/delete/insert */
	bool			catalogtable;				/* 是否系统表 */
	bool			toasttable;					/* 是否toast表(RELKIND_TOASTVALUE) */
	bool			normaltable;				/* 是否普通数据表(RELKIND_RELATION) */
	bool			cananalyse;					/* 是否有解析结果 */
	TupleDesc		tupdesc;					/* 暂存元组描述符指针 */
	XLogRecPtr		start_lsn;					/* 这个wal记录的开始lsn */

	/* DDL解析 */
}TransactionEntry;

/*
 * 所有的事务都会记录在这个结构里
 */
typedef struct WalminerAnalyseProcess
{
	List				*xactlist; 			/* 记录所有的事务的列表 */

	TransactionId 		current_xid;		/*快速搜索xid*/
	TransactionEntry	*current_entry;		/*快速搜索entry*/

	HTAB				*imageStoreHash;	/* 记录image列表的hash */
	HTAB				*tupleDescHash;		/* 所有创建过desc的表的desc hash */
	HTAB				*relPkeyHash;		/* 所有使用过的带有主键的表的hash */
	
	/*
	 * 此处准备用来存储RUNNING_XACT信息，这个信息涌来判断一个事务是否完整
	 * (确定事务是在我们解析wal日志范围内开始的)
	 */
	List				*snapshot;

	List				*missd_relfilenode;			/* 存储在数据字典中找不到的relfilenode */

	ListCell			*ckp_cell;	/* 下一个ckp点 */
	bool				ckp_valid;	/* ckp_cell记录的ckp是否有效 */

}WalminerAnalyseProcess;

/*
 * 记录检索wal日志的信息
 */
typedef struct WalminerSearchWal{
	XLogRecPtr	cur_search_lsn;		/* wal检索读取器，最新完成search的wal日志lsn */
	XLogRecPtr	read_lsn;			/* wal检索读取器,开始读取的LSN */
	XLogRecPtr	decode_lsn;			/* wal解析读取器,开始读取的LSN */
	XLogRecPtr	analyse_lsn;		/* wal解析读取器,开始解析的LSN */
	XLogRecPtr	end_lsn;			/* 
									 * get_search_end为false时，记录为应该停止search的lsn，这时供search线程使用
									 * get_search_end为true时，记录为最后一个record的结束lsn + 1，这时供decode线程使用
									 * 被mutex_search_write保护
									 */
	bool		get_decode_lsn;		/* decode_lsn是否就绪，如果为true，那么解析读取器就开始工作 */
	bool		get_search_end;		/* 是否完成search 
									 * 被mutex_search_write保护*/
	ListCell	*decode_ckp_cell; 			/* 记录decode_lsn时，获取的最新checkpoint */

	TimeLineID	timeline_id; 		/* decode_lsn获取到时的timeline */
	int			wal_seg_size;		/* decode_lsn获取到时的wal_seg_size */

	TransactionId	first_sure_xid;	/* 确定是在给定的wal日志中分配的第一个事务ID */

	bool		iscommit;			/* 在xid解析类型中，记录xid是否提交 */
	bool		isabort;			/* 在xid解析类型中，记录xid是否abort */

	TimestampTz	last_time_searched; /* 记录最近一次检索到的时间 */
	XLogRecPtr	last_time_searched_start_lsn;	/* 记录最近一次检索到的时间记录，对应的开始lsn */
	XLogRecPtr	last_time_searched_end_lsn;	/* 记录最近一次检索到的时间记录，对应的结束lsn */

	List		*checkpoint_list;
	ListCell	*cell_cursor;		/* 指向当前checkpoint */
}WalminerSearchWal;

/*管理解析过程的全局信息*/
typedef struct WalmierDecodingContext{
	MemoryContext			context;

    XLogReaderState 		*reader;		/*wal日志解析读取器*/
	XLogReaderState 		*reader_search;	/*wal日志检索读取器*/
	
	Oid						walminer_contents_oid;	/*记录输出临时表的reloid*/
	Oid						dict_dboid;		/*数据库筛选*/

	void					*search_prive;
	void					*decode_prive;

	WalminerSearchWal		swpro;			/*检索结果记录器*/
	WalminerAnalyseProcess 	anapro;			/*解析结果记录器*/
}WalmierDecodingContext;

typedef struct WalminerDecode
{
	int				max_records_per_tx; /* 在解析开始时，记录下auto_record_in_memory */

	WalminerCallbk	w_call_funcs;

	bool            logout;
	FILE			*logfp;
	FILE			*debugfp;
	FILE			*failurefp;
	FILE			*failurefp_temp;
}WalminerDecode;

extern int				auto_record_in_memory;
extern WalminerDecode  	wdecoder;
extern bool 			regression_mode;
extern MemoryContext	walminer_context;
extern WalmierDecodingContext  *walminer_decode_context;

void* decode_wal(void);
void clean_image(void);
void create_walminer_decode_context(void);

void out_page_to_file(char *page, Oid relfilenode, BlockNumber blkno, int kind);

void wait_thread(void);
void create_walminer_thread(void*(*start_rtn)(void*), bool issearch);
void init_thread_info(void);
void lock_walminer_thread(int  locktype);
void unlock_walminer_thread(int  locktype);
void big_transaction_indraft(TransactionEntry *te);
TransactionEntry* get_transaction_from_list(TransactionId xid, XLogRecPtr origlsn);
void remove_transaction_entry_from_list(TransactionEntry *te);
void big_transaction_spill(TransactionEntry *te, bool focus);
void free_change_list(List *change_list);
bool filter_in_decode(RelFileNode *target_node);

void walminer_context_manage(void);
void self_load_dic_and_wal(void);
void walminer_stop_internal(void);
void pg_walminer(PG_FUNCTION_ARGS);
#endif