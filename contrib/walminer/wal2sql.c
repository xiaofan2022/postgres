/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wal2sql.c
 * 解析模式：
 *		a.解析add的连续wal日志
 *		b.解析指定时间范围内的wal日志(支持前探checkpoint)，这个时间指事务相关wal日志内记录的时间。
 *		因此解析结果可能会比预想的解析结果多，这个参数是方便测试使用或者DBA人为甄别数据使用。
 *		c.解析指定LSN范围内的wal日志(支持前探checkpoint)，精确解析范围，自动化环境建议使用这种方式。
 *		d.解析指定事务ID的wal日志(支持前探checkpoint)，精确解析范围
 *
 * 前探checkpoint模式：
 * 		根据入参界定开始解析的LSN,在这个LSN的基础上向前查找checkpoint点，并查找其redo点。从redo点
 * 		开始维护image信息,从LSN点开始解析redord。必须找到解析点之前的checkpoint点，才能进行解析,
 * 		否则报错。这是为了保证解析范围内的wal日志，都有完整的输出结果。
 * 
 * 后探checkpoint模式(暂不支持):
 * 		根据入参界定开始解析的LSN,在这个LSN的基础上向后查找checkpoint点，并查找其redo点(这个redo点
 * 		可能小于开始解析LSN,这时需要查找下一个checkpoint点)。从找到的redo点开始解析，这种解析方式
 * 		结果比期望的值可能要少一些。
 * 
 * 维护事务开始时间:
 * 		通过RUNNING_XACTS，模糊界定事务开始时间，这个用来确定一个事务是否完整 
 *
 *-------------------------------------------------------------------------
 */
#include "wal2sql.h"
#include "datadictionary.h"
#include "walminer_decode.h"
#include "wm_utils.h"
#include "utils/builtins.h"
#include "catalog/namespace.h"
#if (defined PG_VERSION_13)  || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
#include "access/heaptoast.h"
#include "access/detoast.h"
#else
#include "access/tuptoaster.h"
#endif
#include "utils/lsyscache.h"
#include "walminer_contents.h"
#include "wal2sql_ddl.h"
#include "wal2sql_spi.h"
#include "utils/memutils.h"
#include "catalog/pg_control.h"
#ifdef PG_VERSION_9_5
#include "storage/standby.h"
#else
#include "storage/standbydefs.h"
#endif

#define DECODE_MODE_NOMALTABLE             0x01
#define DECODE_MODE_SINGLE_TABLE           0x02

typedef enum DecodeType
{
    DECODE_TYPE_ALL = 0,
    DECODE_TYPE_LSN,
    DECODE_TYPE_XID,
    DECODE_TYPE_TIME
}DecodeType;

typedef struct ToastTuple
{
	Oid			chunk_id;		/* toast_table.chunk_id */
	int32		chunk_seq; /* toast_table.chunk_seq of the last chunk we
								 * have seen */
	char		*chunk_data;			/* linked list of chunks */
	int			datalength;
} ToastTuple;

typedef struct CheckpointInfo
{
	XLogRecPtr	chp_start_lsn;		/* checkpoint动作开始的LSN*/
	XLogRecPtr	chp_end_lsn;		/* checkpoint记录的lsn*/
	XLogRecPtr	next_chp_start_lsn;	/* 下一个checkpoint的chp_start_lsn - 1 */
}CheckpointInfo;

typedef	struct	Wal2sqlStruct
{
    XLogRecPtr     		lsn_start;	/* 开始LSN，为函数指定 */
    XLogRecPtr      	lsn_end;	/* 结束LSN，为函数指定 */
    TimestampTz     	ts_start;	/* 开始时间，最终解析为LSN，为函数指定 */
    TimestampTz     	ts_end;		/* 结束时间，最终解析为LSN，为函数指定 */
    TransactionId   	xid;		/* 解析xid，为函数指定 */
    Oid             	reloid;		/* 支持单表解析的表Oid, 此处不是relfilenode，为函数指定*/
	Oid					relfilenode;
	bool	            f_search;			/* 前探模式 */
	DecodeType      	at;					/* DecodeType--analyse type */
	int             	am;					/* 解析模式,全部普通表或者单表解析 */
	char            	*ts_start_ptr;		/* 解析时间入参的临时变量 */
    char            	*ts_end_ptr;		/* 解析时间入参的临时变量 */
}Wal2sqlStruct;

PG_FUNCTION_INFO_V1(wal2sql_internal);
PG_FUNCTION_INFO_V1(wal2sql_self_apply);
PG_FUNCTION_INFO_V1(wal2sql_with_catalog);
PG_FUNCTION_INFO_V1(wal2sql_with_ddl);

#define WALMINER_MISSING_DATA   "(WALMINER_DATA_MISSED)"

#define PG_GETARG_TRANSACTIONID(n)	DatumGetTransactionId(PG_GETARG_DATUM(n))

static bool check_varlena(Datum attr,struct varlena** att_return, List *toastlist);
static void analyse_get_relname(StringInfo buffer, TransactionEntry *te);
static char* add_single_quote_from_str(char* strPara);
static void keep_digit_from_str(char* strPara);
static void delete_que_from_str(char* strPara);
static int count_char_in_string(char* str, char ch);

static void handle_miner_result(TransactionEntry *te);
static void self_apply_tuple(TransactionEntry *te);
static bool collect_info_by_change(ReorderBufferChange *change, TransactionEntry *te);
static void collect_toast(ReorderBufferChange *change, TransactionEntry *te, ReorderBufferTupleBuf *tuplebuf);

static void analyse_update(ReorderBufferChange *change, TransactionEntry *te, bool forundo);
static void analyse_insert(ReorderBufferChange *change, TransactionEntry *te, bool forundo);
static void analyse_delete(ReorderBufferChange *change, TransactionEntry *te, bool forundo);

static void init_sql_record(TransactionEntry *te);
static void reset_sql_record(TransactionEntry *te);

static void wal2sql_handle_argument(FunctionCallInfo fcinfo);
static ListCell* get_first_valid_ckp(void);
static void walminer_search_record(XLogReaderState *record);
static void set_call_back_funcs(void);
static void wal2sql_prepare(void);
static void wal2sql_end(void);
static void init_wal2sql_struct(void);

/* 回调函数 */
static void* wal2sql_search_wal(void* temp);
static bool wal2sql_wait_search(void);
static bool wal2sql_check_statue(void);
static void wal2sql_decode_commit(WalRecordBuffer *buf, xl_xact_parsed_commit *parsed, TransactionId xid);
static bool wal2sql_filter_in_decode(RelFileNode *target_node, Oid reloid);
static void wal2sql_front_read(void);


Wal2sqlStruct		wal2sql_struct;
bool				self_apply = false;
bool				with_catalog = false;
bool				with_ddl = false;

static void
init_wal2sql_struct(void)
{
	memset(&wal2sql_struct, 0, sizeof(Wal2sqlStruct));
	wal2sql_struct.am = DECODE_MODE_NOMALTABLE;
	wal2sql_struct.at = DECODE_TYPE_ALL;
}

static void
set_call_back_funcs(void)
{
	wdecoder.w_call_funcs.walminer_search_wal = wal2sql_search_wal;
	wdecoder.w_call_funcs.walminer_wait_search = wal2sql_wait_search;
	wdecoder.w_call_funcs.walminer_check_statue = wal2sql_check_statue;
	wdecoder.w_call_funcs.walminer_decode_commit = wal2sql_decode_commit;
	wdecoder.w_call_funcs.walminer_filter_in_decode = wal2sql_filter_in_decode;
	wdecoder.w_call_funcs.walminer_front_read = wal2sql_front_read;
	wdecoder.w_call_funcs.walminer_wait_thread = wait_thread;
	wdecoder.w_call_funcs.walminer_handle_argument = wal2sql_handle_argument;
}


static void
wal2sql_handle_argument(FunctionCallInfo fcinfo)
{
	bool		gettime = false;
	bool		getlsn = false;
	bool		getxid = false;

	/*
	 * 获取参数
	 */
	if(PG_GETARG_DATUM(0))
	{
		wal2sql_struct.ts_start_ptr = text_to_cstring(PG_GETARG_TEXT_P(0));
	}
	if(PG_GETARG_DATUM(1))
	{
		wal2sql_struct.ts_end_ptr = text_to_cstring(PG_GETARG_TEXT_P(1));
	}
	wal2sql_struct.lsn_start = (XLogRecPtr)(PG_GETARG_INT64(2));
	wal2sql_struct.lsn_end = (XLogRecPtr)(PG_GETARG_INT64(3));
	wal2sql_struct.xid = PG_GETARG_TRANSACTIONID(4);
	wal2sql_struct.f_search = PG_GETARG_BOOL(5);
	wal2sql_struct.reloid = PG_GETARG_OID(6);
	wdecoder.logout = PG_GETARG_BOOL(7);
	wdecoder.max_records_per_tx = auto_record_in_memory;

	if(with_ddl && !wal2sql_struct.f_search)
	{
		elog(ERROR, "ddl analyse should be in a accurate mode");
	}

	if(wal2sql_struct.ts_start_ptr)
	{
		wal2sql_struct.ts_start  =  DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
													CStringGetDatum(wal2sql_struct.ts_start_ptr),
													ObjectIdGetDatum(InvalidOid),
															Int32GetDatum(-1)));
		gettime = true;
		wal2sql_struct.at = DECODE_TYPE_TIME;
	}
	
	if(wal2sql_struct.ts_end_ptr)
	{
		wal2sql_struct.ts_end  =  DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
													CStringGetDatum(wal2sql_struct.ts_end_ptr),
													ObjectIdGetDatum(InvalidOid),
															Int32GetDatum(-1)));
		gettime = true;
		wal2sql_struct.at = DECODE_TYPE_TIME;
	}


	if(0 !=wal2sql_struct.lsn_start || 0 != wal2sql_struct.lsn_end)
	{
		getlsn = true;
		wal2sql_struct.at = DECODE_TYPE_LSN;
	}

	if(0 != wal2sql_struct.xid)
	{
		getxid = true;
		wal2sql_struct.at = DECODE_TYPE_XID;
	}

	if( (gettime && getlsn) || (gettime && getxid) || (getxid && getlsn))
	{
		elog(ERROR, "Can only point time, lsn or xid in a time");
	}

	if(wal2sql_struct.reloid)
	{
		/*单表解析不支持系统表*/
		if(FirstNormalObjectId > wal2sql_struct.reloid)
		{
			elog(ERROR,"Single table analyse can support user table only.");
		}
		wal2sql_struct.relfilenode = get_relfilenodeby_reloid(wal2sql_struct.reloid);
		if(-1 == wal2sql_struct.relfilenode)
		{
			elog(ERROR,"Can not find reloid %u in datadictionary", wal2sql_struct.reloid);
		}
		if(!relkind_is_normalrel(get_relkind_by_reloid(wal2sql_struct.reloid)))
		{
			elog(ERROR,"Single table analyse can support table only.");
		}
		
		wal2sql_struct.am = DECODE_MODE_SINGLE_TABLE;
	}
	else
	{
		wal2sql_struct.am = DECODE_MODE_NOMALTABLE;
	}
	/* debug的log会优先放到配置的debug文件中，否则输入logfile中 */
	if(wdecoder.logout || debug_mode)
	{
		wdecoder.logfp = prepare_logfile();
	}

	if(debug_mode)
	{
		wdecoder.debugfp = prepare_debugfile();
	}

	if(wdecoder.logout)
	{
		if(wal2sql_struct.ts_start_ptr)
			walminer_elog("time1=%s", timestamptz_to_str(wal2sql_struct.ts_start));
		if(wal2sql_struct.ts_end_ptr)
			walminer_elog("time2=%s", timestamptz_to_str(wal2sql_struct.ts_end));

		walminer_elog("lsn1=%x/%x,lsn2=%x/%x,", (uint32)(wal2sql_struct.lsn_start >> 32), (uint32)wal2sql_struct.lsn_start,
												 (uint32)(wal2sql_struct.lsn_end >> 32), (uint32)wal2sql_struct.lsn_end);
		walminer_elog("xid=%u", wal2sql_struct.xid);
		walminer_elog("at=%d, am=%d,reloid=%u", wal2sql_struct.at,wal2sql_struct.am, wal2sql_struct.reloid);
	}
}

/*
 * 收集wal信息的主函数
 *
 * 如果找到end_lsn,调用者会处理这个信息
 */
static void
walminer_search_record(XLogReaderState *record)
{
	TimestampTz 	cur_time = 0;
	uint8			cur_rmgrid = 0;
	uint8			info = 0;
	DecodeType		at = 0;
	TransactionId	xid = InvalidTransactionId;

	cur_rmgrid = XLogRecGetRmid(record);
	at = wal2sql_struct.at;
	walminer_debug("[walminer_search_record] cur_rmgrid=%d", cur_rmgrid);
	switch ((RmgrIds) cur_rmgrid)
	{
		case RM_XLOG_ID:
			info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
			if(XLOG_CHECKPOINT_SHUTDOWN == info || 
						XLOG_CHECKPOINT_ONLINE == info)
			{
				CheckpointInfo	*cpinfo = NULL;
				CheckPoint		*checkPoint = NULL;

				checkPoint = (CheckPoint*)XLogRecGetData(record);

				cpinfo = (CheckpointInfo*)walminer_malloc(sizeof(CheckpointInfo), 0);
				cpinfo->chp_start_lsn = checkPoint->redo;
				cpinfo->chp_end_lsn = record->ReadRecPtr;
				lock_walminer_thread(3);
				walminer_decode_context->swpro.checkpoint_list = 
						lappend(walminer_decode_context->swpro.checkpoint_list, cpinfo);
				walminer_decode_context->swpro.cell_cursor = list_tail(walminer_decode_context->swpro.checkpoint_list);
				unlock_walminer_thread(3);
			}
			else
			{
				return;
			}
			break;
		case RM_XACT_ID:
			/* 
			 * 可以考虑预先收集事务信息，决定是否保留abort事务的decode信息
			 * 认为意义不大，暂时只检查事务时间，来帮助DECODE_TYPE_TIME和
			 * DECODE_TYPE_TID,确定解析lsn
			 */
			
			info = XLogRecGetInfo(record) & XLOG_XACT_OPMASK;
			if(DECODE_TYPE_TIME != at && DECODE_TYPE_XID != at)
			{
				break;
			}
			
			if(XLOG_XACT_COMMIT == info || XLOG_XACT_COMMIT_PREPARED == info)
			{
				xl_xact_commit *xlrec = NULL;

				xlrec = (xl_xact_commit *) XLogRecGetData(record);
				cur_time = xlrec->xact_time;

			}
			else if(XLOG_XACT_ABORT == info || XLOG_XACT_ABORT_PREPARED == info)
			{
				xl_xact_abort *xlrec = NULL;

				xlrec = (xl_xact_abort *) XLogRecGetData(record);
				cur_time = xlrec->xact_time;
			}
			else
			{
				return;
			}

			if(DECODE_TYPE_TIME == at)
			{
				TimestampTz 	last_time = 0;
				XLogRecPtr		last_time_searched_end_lsn = 0;
				//XLogRecPtr		last_time_searched_start_lsn = 0;

				last_time = walminer_decode_context->swpro.last_time_searched;
				if(0 == last_time)
				{
					last_time_searched_end_lsn = 0;
					//last_time_searched_start_lsn = 0;
				}
				else
				{
					last_time_searched_end_lsn = walminer_decode_context->swpro.last_time_searched_end_lsn;
					//last_time_searched_start_lsn =  walminer_decode_context->swpro.last_time_searched_start_lsn;
				}

				
				walminer_decode_context->swpro.last_time_searched = cur_time;
				walminer_decode_context->swpro.last_time_searched_end_lsn = walminer_decode_context->reader_search->EndRecPtr;
				walminer_decode_context->swpro.last_time_searched_start_lsn = walminer_decode_context->reader_search->ReadRecPtr;

				walminer_debug("[walminer_search_record] t_cur=%ld, t_start=%ld",cur_time, wal2sql_struct.ts_start);
				if(cur_time > wal2sql_struct.ts_start && 0 == walminer_decode_context->swpro.decode_lsn)
				{
					/* 现在我们找到了一个大于指定开始时间的时间，我们决定开始decode的lsn */
					if(0 == walminer_decode_context->swpro.last_time_searched_start_lsn)
					{
						walminer_decode_context->swpro.decode_lsn = walminer_decode_context->swpro.read_lsn;
					}
					else
					{
						//当前策略为尽可能多的解析出一些数据，所以定义本事务在解析范围之内
						//选取上一个事务的结束时间作为解析开始点
						walminer_decode_context->swpro.decode_lsn = last_time_searched_end_lsn;
					}
					walminer_debug("[walminer_search_record] decode_lsn=%x/%x",
							(uint32)(walminer_decode_context->swpro.decode_lsn >> 32), (uint32)(walminer_decode_context->swpro.decode_lsn));
				}

				walminer_debug("[walminer_search_record] t_cur=%ld, t_start=%ld",cur_time, wal2sql_struct.ts_end);
				if(cur_time >= wal2sql_struct.ts_end)
				{
					/* 现在我们找到了一个大于指定结束时间的时间，我们决定结束decode的lsn */
					if(0 == walminer_decode_context->swpro.last_time_searched_end_lsn)
					{
						walminer_decode_context->swpro.end_lsn = walminer_decode_context->swpro.read_lsn;
					}
					else
					{
						//选取本事务的结束时间作为解析结束点，本事务数据在解析范围之内
						walminer_decode_context->swpro.end_lsn = 
									walminer_decode_context->swpro.last_time_searched_end_lsn;
					}
					walminer_debug("[walminer_search_record] end_lsn=%x/%x",
							(uint32)(walminer_decode_context->swpro.end_lsn >> 32), (uint32)(walminer_decode_context->swpro.end_lsn));
				}
			}
			else if(DECODE_TYPE_XID == at)
			{
				xid = XLogRecGetXid(record);

				if(xid == wal2sql_struct.xid)
				{
					walminer_decode_context->swpro.iscommit = (XLOG_XACT_COMMIT == info);
					walminer_decode_context->swpro.isabort = (XLOG_XACT_ABORT == info);
					if(walminer_decode_context->swpro.iscommit || walminer_decode_context->swpro.isabort)
					{
						walminer_decode_context->swpro.end_lsn = walminer_decode_context->reader_search->EndRecPtr;
						walminer_debug("[walminer_search_record]xid end_lsn=%x/%x",
							(uint32)(walminer_decode_context->swpro.end_lsn >> 32), (uint32)(walminer_decode_context->swpro.end_lsn));
					}
				}
			}
			break;
		
		case RM_HEAP2_ID:
		case RM_HEAP_ID:
			/* 事务ID解析模式中，判断目标事务ID第一次出现的LSN */
			if(DECODE_TYPE_XID == at)
			{
				xid = XLogRecGetXid(record);
				if(0 == walminer_decode_context->swpro.decode_lsn && xid == wal2sql_struct.xid)
				{
					walminer_decode_context->swpro.decode_lsn = walminer_decode_context->reader_search->ReadRecPtr;
					walminer_debug("[walminer_search_record]xid decode_lsn=%x/%x",
								(uint32)(walminer_decode_context->swpro.decode_lsn >> 32), (uint32)(walminer_decode_context->swpro.decode_lsn));
				}
			}
			break;
		case RM_STANDBY_ID:
			{
				xl_running_xacts *running = (xl_running_xacts *) XLogRecGetData(record);

				info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
				if(XLOG_RUNNING_XACTS != info || 0 != walminer_decode_context->swpro.first_sure_xid)
					return;
				walminer_decode_context->swpro.first_sure_xid = running->nextXid;
				break;
			}
		default:
			break;
	}
}


static void
init_sql_record(TransactionEntry *te)
{
    if(!te->sql.init)
    {
        memset(&te->sql, 0, sizeof(SQLRecord));
        initStringInfo(&te->sql.notsql);
        initStringInfo(&te->sql.sql_pro);
        initStringInfo(&te->sql.sql_undo);
        initStringInfo(&te->sql.sql_temp_1);
        initStringInfo(&te->sql.sql_temp_2);
		te->sql.init = true;
    }
}

static void
reset_sql_record(TransactionEntry *te)
{
	if(te->sql.init)
    {
        resetStringInfo(&te->sql.notsql);
        resetStringInfo(&te->sql.sql_pro);
        resetStringInfo(&te->sql.sql_undo);
        resetStringInfo(&te->sql.sql_temp_1);
        resetStringInfo(&te->sql.sql_temp_2);
    }

	memset(&te->relname, 0, sizeof(NameData));
	te->reloid = 0;
	te->relkind = 0;
	te->sqlkind = 0;
	te->catalogtable = false;
	te->toasttable = false;
	te->normaltable = false;
	te->cananalyse = false;
	te->start_lsn = InvalidXLogRecPtr;
}

static int
count_char_in_string(char* str, char ch)
{
	char	*strPtr = NULL;
	int 	result = 0;
	int 	strlength = 0;
	int 	loop = 0;

	if(!str)
		return result;

	strlength = strlen(str);
	strPtr = str;
	for(;loop < strlength;loop++)
	{
		if(*strPtr == ch)
			result++;
		strPtr++;
	}
	return result;
}

static void
keep_digit_from_str(char* strPara)
{
	int 	strlength = 0,loopo = 0,loopt = 0;
	char*	strtemp = NULL;
	
	if(!strPara)
		return;
	strlength = strlen(strPara);
	strtemp = (char*)palloc0(strlength + 1);
	if(!strtemp)
		elog(ERROR, "Out of memory during keep_digit_from_str");
	
	while(loopo != strlength)
	{
		if(('0' <= strPara[loopo] && '9' >=  strPara[loopo]) || '.' == strPara[loopo])
		{
			strtemp[loopt++] = strPara[loopo++];
		}
		else
		{
			loopo++;
			continue;
		}
	}
	memset(strPara, 0, strlength);
	memcpy(strPara, strtemp, strlen(strtemp));
	pfree(strtemp);
}

static void
delete_que_from_str(char* strPara)
{
	int 	strlength = 0,loopo = 0,loopt = 0;
	char*	strtemp = NULL;
	
	if(!strPara)
		return;
	strlength = strlen(strPara);
	strtemp = (char*)palloc0(strlength + 1);
	if(!strtemp)
		elog(ERROR,"Out of memory during delete_que_from_str");
	
	while(loopo != strlength)
	{
		if((('\'' == strPara[loopo]) && (0 == loopo))
			|| (('\'' == strPara[loopo]) && (strlength - 1 == loopo))
			|| (('\'' == strPara[loopo]) && (strlength - 1 != loopo && (0 != loopo)) && (' ' == strPara[loopo - 1] || ' ' == strPara[loopo + 1])))
		{
			loopo++;
			continue;
		}
		else
		{
			strtemp[loopt++] = strPara[loopo++];
		}
	}
	memset(strPara, 0, strlength);
	memcpy(strPara, strtemp, strlen(strtemp));
	pfree(strtemp);
}

static char*
add_single_quote_from_str(char* strPara)
{
	int 	strlength = 0,loopo = 0,loopt = 0;
	char*	strtemp = NULL;
	int		simplequenum = 0;

	if(!strPara)
		return NULL;

	simplequenum = count_char_in_string(strPara,'\'');
	if(0 >= simplequenum)
		return strPara;
	
	strlength = strlen(strPara);
	strtemp = (char*)palloc0(strlength + simplequenum + 1);
	if(!strtemp)
		ereport(ERROR,(errmsg("Out of memory during addSinglequoteFromStr")));
	
	while(loopo != strlength)
	{
		if('\'' == strPara[loopo])
		{
			strtemp[loopt++] = '\'';
		}
		strtemp[loopt++] = strPara[loopo++];
	}
	pfree(strPara);
	strPara = NULL;
	return strtemp;
}


char*
convert_attr_to_str(Form_pg_attribute fpa,Oid typoutput, bool typisvarlena, Datum attr, List *toast_list)
{
	char	*resultstr = NULL;
    Datum   attr_temp;

    if(typisvarlena)
    {
        struct varlena* val2;

        if(check_varlena((attr), &val2, toast_list))
		{
			attr_temp = PointerGetDatum(val2);
			resultstr = OidOutputFunctionCall(typoutput, attr_temp);
		}
		else
		{
			resultstr = pstrdup("!!!can not found toast value!!!");
		}
    }
    else
    {
        attr_temp = attr;
		resultstr = OidOutputFunctionCall(typoutput, attr_temp);
    }

	if(TSVECTOROID == fpa->atttypid || TSQUERYOID == fpa->atttypid)
	{
		delete_que_from_str(resultstr);
	}
	if(CASHOID == fpa->atttypid)
	{
		keep_digit_from_str(resultstr);
	}
	else if(JSONOID == fpa->atttypid || TEXTOID == fpa->atttypid || BPCHAROID == fpa->atttypid || VARCHAROID == fpa->atttypid
		|| XMLOID == fpa->atttypid || NAMEOID == fpa->atttypid || JSONBOID == fpa->atttypid || CHAROID == fpa->atttypid
		|| 199 == fpa->atttypid || TEXTARRAYOID == fpa->atttypid || 1014 == fpa->atttypid || 1015 == fpa->atttypid
		|| 143 == fpa->atttypid || 1003 == fpa->atttypid || 3807 == fpa->atttypid || 1002 == fpa->atttypid)
	{
		resultstr = add_single_quote_from_str(resultstr);
	}
	return resultstr;
}

static bool 
check_varlena(Datum attr,struct varlena** att_return, List *toastlist)
{
	text        *attr_text = NULL;
	ToastTuple    *tent = NULL;
	struct varlena *attr_varlena = NULL;
	struct varlena *result = NULL;
	struct varatt_external toast_pointer;
	int32		ressize = 0;
	bool		gettoastdata = false;
    ListCell       *cell = NULL;
	
	attr_text = (text*)DatumGetPointer(attr);
	attr_varlena = (struct varlena *)attr_text;

	if(!VARATT_IS_EXTERNAL_ONDISK(attr_varlena))
	{
		*att_return = attr_varlena;
		return true;
	}

	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
#if ((defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16))
	ressize = VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer);
#else
	ressize = toast_pointer.va_extsize;
#endif
	result = (struct varlena *) palloc(ressize + VARHDRSZ);

	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
		SET_VARSIZE_COMPRESSED(result, ressize + VARHDRSZ);
	else
		SET_VARSIZE(result, ressize + VARHDRSZ);

	cell = list_head(toastlist);
	while(cell)
	{
        tent = lfirst(cell);
		if(tent->chunk_id == toast_pointer.va_valueid)
		{
			gettoastdata = true;
  			memcpy(VARDATA(result) + tent->chunk_seq * TOAST_MAX_CHUNK_SIZE, tent->chunk_data, tent->datalength);
		}
		
#if (defined PG_VERSION_13)  || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
        cell = lnext(toastlist, cell);
#else
        cell = lnext(cell);
#endif
	}
	if(!gettoastdata)
	{
		elog(WARNING, "get a attribute value that can not find toast valueid %u in toast relation %u",
			toast_pointer.va_valueid, toast_pointer.va_toastrelid);
		return false;
	}
	if (VARATT_IS_COMPRESSED(result))
	{
		struct varlena *tmp = result;
#if (defined PG_VERSION_13)  || (defined PG_VERSION_14)  || (defined PG_VERSION_15) || (defined PG_VERSION_16)
        result = detoast_attr(tmp);
#else
		result = heap_tuple_untoast_attr(tmp);
#endif	
    	pfree(tmp);
	}
	*att_return = result;
	return true;
}

static ToastTuple*
make_toast_tuple(int datalength, char* data, Oid id, int seq)
{
    ToastTuple  *tt = NULL;
    char        *ptr = NULL;

    ptr = walminer_malloc(datalength + sizeof(ToastTuple), 0);
    tt = (ToastTuple*)ptr;

    tt->chunk_id = id;
    tt->chunk_seq = seq;
    tt->datalength = datalength;
    tt->chunk_data = ptr + sizeof(ToastTuple);
    memcpy(tt->chunk_data, data, datalength);
    
    return tt;
}

static void
print_literal(StringInfo s, Oid typid, char *outputstr)
{
	const char *valptr;

	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			/* NB: We don't care about Inf, NaN et al. */
			appendStringInfoString(s, outputstr);
			break;

		case BITOID:
		case VARBITOID:
			appendStringInfo(s, "B'%s'", outputstr);
			break;

		case BOOLOID:
			if (strcmp(outputstr, "t") == 0)
				appendStringInfoString(s, "true");
			else
				appendStringInfoString(s, "false");
			break;

		default:
			appendStringInfoChar(s, '\'');
			for (valptr = outputstr; *valptr; valptr++)
			{
				char		ch = *valptr;

				if (SQL_STR_DOUBLE(ch, false))
					appendStringInfoChar(s, ch);
				appendStringInfoChar(s, ch);
			}
			appendStringInfoChar(s, '\'');
			break;
	}
}

static void
analyse_get_relname(StringInfo buffer, TransactionEntry *te)
{
    appendStringInfoString(buffer, quote_identifier(te->namespace.data));
    appendStringInfoString(buffer, ".");
    appendStringInfoString(buffer, quote_identifier(te->relname.data));
}

/*
 * 在解析一个表相关的record时，使用此函数收集一些表相关的信息
 * 返回值表示是否继续解析此record，目前不解析pg_statistic表
 */
static bool
collect_info_by_change(ReorderBufferChange *change, TransactionEntry *te)
{
    Oid         reloid = InvalidOid;

    reloid = get_reloid_by_relfilenode(GET_CHANGE_RELNODE(change));
    Assert(0 != reloid);

    /* 如果不需要解析系统表，且当前为系统表，则不解析这个record */
    if(!with_catalog && !with_ddl && FirstNormalObjectId > reloid)
    {
        return false;
    }

    te->reloid = reloid;
    te->relkind = get_relkind_by_reloid(reloid);

    if(!get_relname_by_reloid(te->reloid, &te->relname))
    {
        elog(ERROR, "Can not find reloid %u in data dictionary", te->reloid);
    }

    if(!get_nspoid_by_reloid(te->reloid, &te->nspoid))
    {
        elog(ERROR, "Can not find reloid %u in data dictionary", te->reloid);
    }

    if(!get_nsp_by_nspoid(te->nspoid, &te->namespace))
    {
        elog(ERROR, "Can not find nspoid %u in data dictionary", te->nspoid);
    }

    if(0 == strcmp("pg_statistic", te->relname.data))
		return false;
    
    te->tupdesc = get_desc_by_reloid(te->reloid);

    if(table_is_catalog_relation(te->relname.data, te->reloid))
    {
        te->catalogtable = true;
        te->has_catalog_changes = true;

        Assert(!with_ddl || !with_catalog);

        /*
         * 严格来说，将这个调用放在indeset/update/delete各自的函数中比较好 
         * 目前看来放在这也没有什么不利因素，因此先放在这里。
         * 
         * ddl解析模式下，不支持with_catalog，因此处理完系统表后，可以直接返回
         */
        if(with_ddl)
        {
            ddl_handle(change, te);
            return false;
        }
        /* 如果当前需要解析系统表，直接返回需要解析此record */
        else if(with_catalog)
        {
            return true;
        }
        else
            return false;
    }
    else if(relkind_is_toastrel(te->relkind))
    {
        te->toasttable = true;
    }
    else if(relkind_is_normalrel(te->relkind))
    {
         te->normaltable = true;
    }

    /* 过滤索引，序列，外部表等 */
    if(!te->normaltable && !te->toasttable)
        return false;
    return true;
}


static void
self_apply_tuple(TransactionEntry *te)
{
	bool	ret = false;

	if(!te->sql.init)
	{
		/* 不应该发生 */
		elog(ERROR, "Why te->sql did not inited?");
	}

	if(!te->sql.issql)
	{
		return;
	}

	walminer_record_in_temp(te->sql.sql_pro.data);
	if(te->alive)
	{
		ret = walminer_spi_execute(te->sql.sql_pro.data, te->sqlkind);
		te->alive = ret;
	}
}

static void
handle_miner_result(TransactionEntry *te)
{
	if(self_apply)
	{
		/* 系统表不参与自apply解析 */
		if(te->catalogtable)
			return;
		self_apply_tuple(te);
	}
	else
	{
		insert_walcontents_tuple((void*)te);
	}
}

static void
wal2sql_decode_commit(WalRecordBuffer *buf,
			 xl_xact_parsed_commit *parsed, TransactionId xid)
{
	XLogRecPtr			origin_lsn = InvalidXLogRecPtr;
	TimestampTz 		commit_time = parsed->xact_time;
	RepOriginId 		origin_id = XLogRecGetOrigin(buf->record);
	int					i = 0;
	TransactionEntry 	*te = NULL;
	TransactionId 		xid_loop = 0;
	bool				issubtransaction = false;
	bool				tx_alive = true;
	MemoryContext		oldcontext = NULL;
	ResourceOwner		oldowner = NULL;

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		origin_lsn = parsed->origin_lsn;
		commit_time = parsed->origin_timestamp;
	}
		
	if(self_apply)
	{
		walminer_spi_tx(DECODE_SQL_KIND_BEGIN, oldcontext, oldowner);
	}
	xid_loop = xid;
	while(true)
	{
		bool		inanalyse = true;
		walminer_debug("[wal2sql_decode_commit] xid=%u 1", xid_loop);
		/* 第一次循环时xid_loop为父事务ID */
		te = get_transaction_from_list(xid_loop, InvalidXLogRecPtr);

		/* 前探解析模式，删除decode_lsn->analyse_lsn之间的数据 */
		if(wal2sql_struct.f_search)
			walminer_debug("[front_read]walminer_decode_context->swpro.analyse_lsn=%x/%x", 
				(uint32)(walminer_decode_context->reader->ReadRecPtr >> 32), (uint32)(walminer_decode_context->reader->ReadRecPtr));
		if(wal2sql_struct.f_search &&
			walminer_decode_context->reader->ReadRecPtr < walminer_decode_context->swpro.analyse_lsn)
		{
			/* 前探解析模式中，当前wal记录时不需要解析结果的wal日志 */
			inanalyse = false;
		}

		if (te == NULL)
		{
			if(issubtransaction)
			{
				/* 子事务发生abort，会产生空子事务，此处不能return */
				xid_loop = parsed->subxacts[i++];
				continue;
			}
		}
		else if(!te->changes && !te->disked)
		{
			/* 如果事务内没有记录changes，只需要移除te */
			remove_transaction_entry_from_list(te);

			/* 如果父事务没有有效数据，此事需要创建解析子事务的路径 */
			if(!issubtransaction && 0 < parsed->nsubxacts)
			{
				issubtransaction = true;
			}
		}
		else
		{
			if(self_apply)
				te->alive = tx_alive;
			te->final_lsn = buf->origptr;
			te->end_lsn = buf->endptr;
			te->commit_time = commit_time;
			te->origin_id = origin_id;
			te->origin_lsn = origin_lsn;

			if(i < parsed->nsubxacts)
			{
				xid_loop = parsed->subxacts[i];
				if(issubtransaction)
				{
					te->issubtransaction = true;
					te->topxid = xid;
					i++;
				}
				issubtransaction = true;
			}
			init_sql_record(te);
			if(inanalyse)
			{
				wal2sql_commit_single_transaction(te);
				if(self_apply)
				{
					tx_alive = te->alive;
				}
			}
			remove_transaction_entry_from_list(te);
		}
		if(i == parsed->nsubxacts || !issubtransaction)
			break;
	}
	if(self_apply)
	{
		if(tx_alive)
			tx_alive &= walminer_spi_tx(DECODE_SQL_KIND_COMMIT, oldcontext, oldowner);
		else
			tx_alive &= walminer_spi_tx(DECODE_SQL_KIND_ABORT, oldcontext, oldowner);
		walminer_handle_failure_transaction(tx_alive);
	}
}

/* 在前探解析中，获取开始decode的lsn */
static void
wal2sql_front_read(void)
{
	CheckpointInfo	*ckpinfo = NULL;

	if(!wal2sql_struct.f_search)
		return;
	walminer_decode_context->swpro.analyse_lsn = walminer_decode_context->swpro.decode_lsn;
	walminer_debug("[front_read]walminer_decode_context->swpro.analyse_lsn=%x/%x", 
		(uint32)(walminer_decode_context->swpro.analyse_lsn >> 32), (uint32)(walminer_decode_context->swpro.analyse_lsn));
	
	if(DECODE_TYPE_ALL == wal2sql_struct.at)
	{
		elog(ERROR, "Only time,lsn,xid analyse mode can support front search.");
	}
	
	if(walminer_decode_context->swpro.decode_ckp_cell)
	{
		ckpinfo = lfirst(walminer_decode_context->swpro.decode_ckp_cell);

		if(ckpinfo->chp_start_lsn >= walminer_decode_context->swpro.read_lsn)
		{
			walminer_decode_context->swpro.decode_lsn = ckpinfo->chp_start_lsn;
			walminer_debug("[front_read]walminer_decode_context->swpro.decode_lsn=%x/%x", 
		(uint32)(walminer_decode_context->swpro.decode_lsn >> 32), (uint32)(walminer_decode_context->swpro.decode_lsn));
			
			return;
		}
	}
	elog(ERROR, "Fail to search checkpoint in front search mode");
}

void
wal2sql_commit_single_transaction(TransactionEntry *te)
{
	ReorderBufferChange *change = NULL;
	ListCell			*cell = NULL;

	if(te->disked)
	{
		/* 
		 * 如果当前已经有change刷新到磁盘中了，那么在analyse之前，将现存的changes
		 * 先刷新到磁盘，便于代码处理，此处有优化空间
		 */
		walminer_debug("[commit_single_transaction]last disk");
		big_transaction_spill(te, true);
	}

	init_ddl_analyse();
	while(true)
	{
		if(te->disked)
		{
			
			if(!te->finish_indraft)
			{
				/* 此时te->changes一定是空的，需要重新从磁盘获取数据 */
				walminer_debug("[commit_single_transaction]big_transaction_indraft");
				big_transaction_indraft(te);
				walminer_debug("[commit_single_transaction]%d",te->nentries_mem);
			}
			else
			{
				/* 此时磁盘也没有数据了，说明这个事务就解析完成了 */
				break;
			}
		}
		cell = NULL;
		/* 提取toast值，由于DELETE的toast WAL记录在主记录后面，必须先将toast记录解析出来 */
		foreach(cell, te->changes)
		{
			change = lfirst(cell);
			switch (change->action)
			{
				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
				case REORDER_BUFFER_CHANGE_INSERT:
					collect_toast(change, te, change->data.tp.newtuple);
					break;
				case REORDER_BUFFER_CHANGE_DELETE:
					collect_toast(change, te, change->data.tp.oldtuple);
					break;
				default:
					break;
			}
		}
		foreach(cell, te->changes)
		{
			change = lfirst(cell);
			walminer_debug("[reorder_buffer_commit] %d", change->action);
			te->start_lsn = change->lsn;
			switch (change->action)
			{
				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
					/* 目前不需要解析 */
					break;
				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
				case REORDER_BUFFER_CHANGE_INSERT:
					walminer_debug("[reorder_buffer_commit] analyse_insert");
					te->sql.issql = true;
					if(!collect_info_by_change(change, te))
        				break;
					analyse_insert(change, te, false);
					resetStringInfo(&te->sql.sql_temp_1);
					resetStringInfo(&te->sql.sql_temp_2);
					analyse_delete(change, te, true);
					te->sqlkind = DECODE_SQL_KIND_INSERT;
					if(!relkind_is_toastrel(te->relkind))
						handle_miner_result(te);
					te->sql.issql = false;
					break;
				case REORDER_BUFFER_CHANGE_DELETE:
					walminer_debug("[reorder_buffer_commit] analyse_delete");
					te->sql.issql = true;
					if(!collect_info_by_change(change, te))
        				break;
					analyse_delete(change, te, false);
					resetStringInfo(&te->sql.sql_temp_1);
					resetStringInfo(&te->sql.sql_temp_2);
					analyse_insert(change, te, true);
					te->sqlkind = DECODE_SQL_KIND_DELETE;
    				if(!relkind_is_toastrel(te->relkind))
						handle_miner_result(te);
					te->sql.issql = false;
					break;
				case REORDER_BUFFER_CHANGE_UPDATE:
					walminer_debug("[reorder_buffer_commit] analyse_update");
					te->sql.issql = true;
					if(!collect_info_by_change(change, te))
        				break;
					analyse_update(change, te, false);
					resetStringInfo(&te->sql.sql_temp_1);
					resetStringInfo(&te->sql.sql_temp_2);
					analyse_update(change, te, true);
					te->sqlkind = DECODE_SQL_KIND_UPDATE;
    				handle_miner_result(te);
					te->sql.issql = false;
					break;
#ifndef	PG_VERSION_9_5
				case REORDER_BUFFER_CHANGE_MESSAGE:
					/* 不需要解析 */
					break;
#endif
				case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
					/* TODO(lchch) */
					break;
				case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
					/* TODO(lchch) */
					break;
				default:
					break;
			}
			reset_sql_record(te);
			
			te->nentries_mem--;
			walminer_debug("[commit_single_transaction]xiao hao hou %d", te->nentries_mem);
		}

		if(te->changes)
		{
			/* 清空当前内存里的changes,这些已经完成analyse */
			free_change_list(te->changes);
			te->changes = NULL;
		}
		if(!te->disked)
		{
			/* 没有落盘的事务，可以直接结束 */
			break;
		}
	}
}

static void
analyse_insert(ReorderBufferChange *change, TransactionEntry *te, bool forundo)
{
    TupleDesc	            tupdesc = NULL;
    StringInfo              s_sum = NULL;
    StringInfo              s_att = &te->sql.sql_temp_1;
    StringInfo              s_value = &te->sql.sql_temp_2;
    int                     natt = 0;
    bool                    get_first_att = false;
    HeapTupleData           *tuple = NULL;
    ReorderBufferTupleBuf   *tuplebuf = NULL;

    tupdesc = te->tupdesc;
    Assert(tupdesc);

    /* self_apply模式下，不需要redo语句 */
    if(self_apply && forundo)
    {
        return;
    }

    if(!forundo)
    {
        s_sum = &te->sql.sql_pro;
        tuplebuf = change->data.tp.newtuple;
        te->cananalyse = true;
        Assert(tuplebuf);
        walminer_debug("[analyse_insert][]kind=%d", te->relkind);
    }
    else
    {
        s_sum = &te->sql.sql_undo;
        tuplebuf = change->data.tp.oldtuple;
        if(tuplebuf)
        {
            te->cananalyse = true;
        }
        else
        {
            te->cananalyse = false;
        }
        walminer_debug("[analyse_insert][undo]kind=%d", te->relkind);
    }
    
    tuple = &tuplebuf->tuple;

    if(relkind_is_toastrel(te->relkind))
    {
        walminer_debug("-------INSERT INTO:%s[toast]", te->relname.data);
        return;
    }

    for (natt = 0; natt < tupdesc->natts; natt++)
	{
        Form_pg_attribute attr; /* the attribute itself */
		Oid			typid;		/* type of current attribute */
		Oid			typoutput;	/* output function */
		bool		typisvarlena;
		Datum		origval;	/* possibly toasted Datum */
		bool		isnull;		/* column is null? */

        if(!te->cananalyse)
            break;

        attr = TupleDescAttr(tupdesc, natt);

        /* 如果字段被drop，那么解析结果不出现这个字段 */
        if (attr->attisdropped)
			continue;
        
        if(!get_first_att)
        {
            appendStringInfoString(s_att, "(");
            appendStringInfoString(s_att, quote_identifier(NameStr(attr->attname)));
            appendStringInfoString(s_value, "(");
        }
        else
        {
            appendStringInfoString(s_att, " ,");
            appendStringInfoString(s_att, quote_identifier(NameStr(attr->attname)));
            appendStringInfoString(s_value, " ,");
        }
        
        typid = attr->atttypid;
        origval = heap_getattr(tuple, natt + 1, tupdesc, &isnull);
        if(!get_typeoutput_fromdic(typid, &typoutput, &typisvarlena))
            elog(ERROR, "Can not find datatype %u", typid);

        if (isnull)
			appendStringInfoString(s_value, "null");
//		else if (typisvarlena && VARATT_IS_EXTERNAL_ONDISK(origval))
//			appendStringInfoString(s_value, "unchanged-toast-datum");
		else
        {
            char *resstr = NULL;

            resstr = convert_attr_to_str(attr, typoutput, typisvarlena, origval, te->toast_list);
			print_literal(s_value, typid, resstr);

			if(resstr)
				walminer_free(resstr, 0);
        }
        get_first_att = true;

    }
    if(!te->cananalyse)
    {
        appendStringInfoString(s_att, WALMINER_MISSING_DATA);
        appendStringInfoString(s_value, WALMINER_MISSING_DATA);
    }
    else
    {
        appendStringInfoString(s_att, ")");
        appendStringInfoString(s_value, ")");
    }

    appendStringInfoString(s_sum, "INSERT INTO ");
    //appendStringInfoString(s_sum, quote_identifier(te->relname.data));
    analyse_get_relname(s_sum,te);
    
    appendStringInfoString(s_sum, s_att->data);
    appendStringInfoString(s_sum, " VALUES");
    appendStringInfoString(s_sum, s_value->data);
    if(!forundo)
        walminer_debug("-------INSERT:%s", s_sum->data);
}

static void
collect_toast(ReorderBufferChange *change, TransactionEntry *te, ReorderBufferTupleBuf *tuplebuf)
{
	Oid             reloid = InvalidOid;
	int				relkind;
    TupleDesc       tupdesc = NULL;
    HeapTupleData   *tuple = NULL;
	Oid		        chunk_id;
	int		        chunk_seq;
	char	        *chunk_data;
	bool	        isnull = false;
	ToastTuple      *tt = NULL;

	if(!tuplebuf)
		return;

    reloid = get_reloid_by_relfilenode(GET_CHANGE_RELNODE(change));
    Assert(0 != reloid);

    /* 如果不需要解析系统表，且当前为系统表，则不解析这个record */
    if(!with_catalog && !with_ddl && FirstNormalObjectId > reloid)
    {
        return;
    }

	relkind = get_relkind_by_reloid(reloid);
	if(!relkind_is_toastrel(relkind))
		return;

	tuple = &tuplebuf->tuple;
    tupdesc = get_desc_by_reloid(reloid);
    Assert(tupdesc);

	chunk_id = DatumGetObjectId(fastgetattr(tuple, 1, tupdesc, &isnull));
	chunk_seq =DatumGetInt32(fastgetattr(tuple, 2, tupdesc, &isnull));
	chunk_data = DatumGetPointer(fastgetattr(tuple, 3, tupdesc, &isnull));

	tt = make_toast_tuple(VARSIZE(chunk_data) - VARHDRSZ, 
							VARDATA(chunk_data), chunk_id, chunk_seq);
	
	te->toast_list = lappend(te->toast_list, tt);
}

static void
analyse_delete(ReorderBufferChange *change, TransactionEntry *te, bool forundo)
{
    TupleDesc               tupdesc = NULL;
    StringInfo              s_sum = NULL;
    int                     natt = 0;
    bool                    get_first_att = false;
    HeapTupleData           *tuple = NULL;
    ReorderBufferTupleBuf   *tuplebuf = NULL;
    int16                   *pklist;
    bool                    get_pklist = false;
    int                     loop = 0;

    /* self_apply模式下，不需要redo语句 */
    if(self_apply && forundo)
    {
        return;
    }
    
    tupdesc = te->tupdesc;
    Assert(tupdesc);

    if(!forundo)
    {
        s_sum = &te->sql.sql_pro;
        tuplebuf = change->data.tp.oldtuple;
        walminer_debug("[analyse_delete][]kind=%d", te->relkind);
    }
    else
    {
        s_sum = &te->sql.sql_undo;
        tuplebuf = change->data.tp.newtuple;
        walminer_debug("[analyse_delete][undo]kind=%d", te->relkind);
    }
    
    if(!tuplebuf)
    {
        te->cananalyse = false;
    }
    else
    {
        te->cananalyse = true;
        tuple = &tuplebuf->tuple;
    }

    if(relkind_is_toastrel(te->relkind))
    {
        walminer_debug("-------DELETE FROM:%s[toast]", te->relname.data);
        return;
    }

    get_pklist = get_pkey_list_by_reloid(te->reloid, &pklist);

    appendStringInfoString(s_sum, "DELETE FROM ");
    //appendStringInfoString(s_sum, quote_identifier(te->relname.data));
    analyse_get_relname(s_sum, te);
    appendStringInfoString(s_sum, " ");
    if(!te->cananalyse)
    {
        appendStringInfoString(s_sum, WALMINER_MISSING_DATA);
    }
    else
    {
        appendStringInfoString(s_sum, "WHERE ");
    }

    for (natt = 0; te->cananalyse && natt < tupdesc->natts; natt++)
	{
        Form_pg_attribute attr; /* the attribute itself */
		Oid			typid;		/* type of current attribute */
		Oid			typoutput;	/* output function */
		bool		typisvarlena;
		Datum		origval;	/* possibly toasted Datum */
		bool		isnull;		/* column is null? */

        if(get_pklist)
        {
            /* 在有主键的情况下，只在条件语句中显示主键部分 */
            if(pklist[loop] == natt + 1)
            {
                loop++;
            }
            else
            {
                /* 跳过非主键 */
                continue;
            }
        }

        attr = TupleDescAttr(tupdesc, natt);

        /* 如果字段被drop，那么解析结果不出现这个字段 */
        if (attr->attisdropped)
			continue;
        
        if(get_first_att)
        {
            appendStringInfoString(s_sum, " AND ");
        }

        appendStringInfoString(s_sum, quote_identifier(NameStr(attr->attname)));
        
        
        typid = attr->atttypid;
        origval = heap_getattr(tuple, natt + 1, tupdesc, &isnull);
        if(!get_typeoutput_fromdic(typid, &typoutput, &typisvarlena))
            elog(ERROR, "Can not find datatype %u", typid);

        if (isnull)
		{
			appendStringInfoString(s_sum, " is ");
			appendStringInfoString(s_sum, "null");
		}
//		else if (typisvarlena && VARATT_IS_EXTERNAL_ONDISK(origval))
//			appendStringInfoString(s_sum, "unchanged-toast-datum");
		else
        {
            char *resstr = NULL;

            resstr = convert_attr_to_str(attr, typoutput, typisvarlena, origval, te->toast_list);
			appendStringInfoString(s_sum, "=");			
			print_literal(s_sum, typid, resstr);

			if(resstr)
				walminer_free(resstr, 0);
        }
        get_first_att = true;
    }

    if(!forundo)
        walminer_debug("-------DELETE:%s", s_sum->data);
}

static void
analyse_update(ReorderBufferChange *change, TransactionEntry *te, bool forundo)
{
    TupleDesc	        tupdesc = NULL;

    StringInfo          s_sum = NULL;
    StringInfo          s_new = &te->sql.sql_temp_1;
    StringInfo          s_old = &te->sql.sql_temp_2;
    int                 natt = 0;
    HeapTupleData       *tuple_new = NULL;
    HeapTupleData       *tuple_old = NULL;
    bool                get_first_att_new = false;
    bool                get_first_att_old = false;
    ReorderBufferTupleBuf *tupbuf_new = NULL;
    ReorderBufferTupleBuf *tupbuf_old = NULL;
    int16                   *pklist;
    bool                    get_pklist = false;
    int                     loop = 0;

    tupdesc = te->tupdesc;
    Assert(tupdesc);

    /* self_apply模式下，不需要redo语句 */
    if(self_apply && forundo)
    {
        return;
    }

    if(!forundo)
    {
        tupbuf_new = change->data.tp.newtuple;
        tupbuf_old = change->data.tp.oldtuple;
        s_sum = &te->sql.sql_pro;
    }
    else
    {
        tupbuf_old = change->data.tp.newtuple;
        tupbuf_new = change->data.tp.oldtuple;
        s_sum = &te->sql.sql_undo;
    }

    if(!tupbuf_new || !tupbuf_old)
    {
        te->cananalyse = false;
    }
    else
    {
        te->cananalyse = true;
    }

    tuple_new = &tupbuf_new->tuple;
    tuple_old = &tupbuf_old->tuple;

    get_pklist = get_pkey_list_by_reloid(te->reloid, &pklist);

    for (natt = 0; te->cananalyse && natt < tupdesc->natts; natt++)
	{
        Form_pg_attribute attr; /* the attribute itself */
		Oid			typid;		/* type of current attribute */
		Oid			typoutput;	/* output function */
		bool		typisvarlena;
		Datum		neworigval;	/* possibly toasted Datum */
        Datum		oldorigval;	/* possibly toasted Datum */
		bool		newisnull;		/* column is null? */
        bool		oldisnull;		/* column is null? */
        bool        same = false;
        char        *newresstr = NULL;
        char        *oldresstr = NULL;


        attr = TupleDescAttr(tupdesc, natt);

        /* 如果字段被drop，那么解析结果不出现这个字段 */
        if (attr->attisdropped)
			continue;

        typid = attr->atttypid;
        neworigval = heap_getattr(tuple_new, natt + 1, tupdesc, &newisnull);
        oldorigval = heap_getattr(tuple_old, natt + 1, tupdesc, &oldisnull);
        if(!get_typeoutput_fromdic(typid, &typoutput, &typisvarlena))
            elog(ERROR, "Can not find datatype %u", typid);
        if(!newisnull)
        {
            newresstr = convert_attr_to_str(attr, typoutput, typisvarlena, neworigval, te->toast_list);
        }
        if(!oldisnull)
        {
            oldresstr = convert_attr_to_str(attr, typoutput, typisvarlena, oldorigval, te->toast_list);
        }

        if(newisnull && oldisnull)
            same = true;
        else if(newisnull || oldisnull)
            same = false;
        else
        {
            Assert(!newisnull && !oldisnull);
            same = (0 == strcmp(newresstr, oldresstr));
        }
        /* for new */
        if(!same)
        {
            if(get_first_att_new)
            {
                appendStringInfoString(s_new, ", ");
            }
            appendStringInfoString(s_new, quote_identifier(NameStr(attr->attname)));
            appendStringInfoString(s_new, "=");
            if (newisnull)
                appendStringInfoString(s_new, "null");
            else
            {
                print_literal(s_new, typid, newresstr);
            }
            get_first_att_new = true;
        }
        
        /* for old */
        if(get_pklist)
        {
            /* 在有主键的情况下，只在田间语句中显示主键部分 */
            if(pklist[loop] == natt + 1)
            {
                loop++;
            }
            else
            {
                /* 跳过非主键 */
                continue;
            }
        }
        if(get_first_att_old)
        {
            appendStringInfoString(s_old, " AND ");
        }
        appendStringInfoString(s_old, quote_identifier(NameStr(attr->attname)));
        if (oldisnull)
		{
			appendStringInfoString(s_old, " is ");
			appendStringInfoString(s_old, "null");
		}
		else
		{
			appendStringInfoString(s_old, "=");
			print_literal(s_old, typid, oldresstr);
        }
        get_first_att_old = true;

		if(newresstr)
			walminer_free(newresstr, 0);
		if(oldresstr)
			walminer_free(oldresstr, 0);
    }

    appendStringInfoString(s_sum, "UPDATE ");
    //appendStringInfoString(s_sum, quote_identifier(te->relname.data));
    analyse_get_relname(s_sum, te);
    appendStringInfoString(s_sum, " ");
    if(te->cananalyse)
    {
        appendStringInfoString(s_sum, "SET ");
        appendStringInfoString(s_sum, s_new->data);
        appendStringInfoString(s_sum, " WHERE ");
        appendStringInfoString(s_sum, s_old->data);
    }
    else
    {
        appendStringInfoString(s_sum, WALMINER_MISSING_DATA);
    }
    if(forundo)
        walminer_debug("-------UPDATE:%s", s_sum->data);
}

void
wal2sql_prepare(void)
{
    if(self_apply)
	{
		walminer_spi_connect();
		walminer_init_failure_file();
		walminer_init_failure_temp_file();
	}
}

static void
wal2sql_search_end(void)
{
	lock_walminer_thread(3);
	walminer_debug("[walminer_search_record]get_search_end=true");
	walminer_decode_context->swpro.end_lsn = walminer_decode_context->reader_search->EndRecPtr;
	walminer_decode_context->swpro.get_search_end = true;
	unlock_walminer_thread(3);
}

/*
 * 检索wal记录维护checkpoint链表
 * 在解析模式DECODE_TYPE_LSN,DECODE_TYPE_TIME中还负责前探工作
 * 在解析模式DECODE_TYPE_XID,DECODE_TYPE_TIME中负责查找开始
 * 解析的lsn。
 */
static void*
wal2sql_search_wal(void* temp)
{
	WalmierDecodingContext		*wdc;
	XLogRecPtr              	first_record = InvalidXLogRecPtr;
	XLogRecPtr					a,c;

	wdc = walminer_decode_context;
	wdc->swpro.read_lsn = prepare_read(true, wal2sql_struct.lsn_end);
	first_record = wdc->swpro.read_lsn;

	switch (wal2sql_struct.at)
	{
		case DECODE_TYPE_ALL:
			/* 输入就要解析 */
			wdc->swpro.decode_lsn = wdc->swpro.read_lsn;
			//search_wal_all();
			break;
		case DECODE_TYPE_LSN:
			/* 
			 * 入参lsn(a->b)与入参wal(c->d)作比较,确定起始lsn
			 */
			a =wal2sql_struct.lsn_start;
			c = wdc->swpro.read_lsn;
			wdc->swpro.decode_lsn = a > c?a:c;
			//search_wal_lsn();
			break;
		case DECODE_TYPE_TIME:
			/* 
			 * 一切都是未确定的，我们需要在search过程中找出wdc->swpro.decode_lsn
			 */
			//search_wal_time();
			break;
		case DECODE_TYPE_XID:
			/* code */
			//search_wal_xid();
			break;
		default:
			elog(ERROR, "Wrong analyse type %d", wal2sql_struct.at);
			break;
	}
#if (defined PG_VERSION_13)  || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
	XLogBeginRead(walminer_decode_context->reader_search, first_record);
#endif
	while(true)
    {
        get_next_record(wdc->reader_search, wdc->search_prive ,first_record);
		wdc->swpro.cur_search_lsn = walminer_decode_context->reader_search->ReadRecPtr;
        first_record = InvalidXLogRecPtr;

		/*
		 * 获取到空的record就说明wal日志读完了;或者有wal文件缺失，丢弃后面
		 * wal;或者读到了入参结束lsn位置
		 * 
		 * 在DECODE_TYPE_LSN/DECODE_TYPE_TIME/DECODE_TYPE_XID解析类型时
		 * 需要别的地方决定end_lsn和get_search_end
		 */
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
		if(!walminer_decode_context->reader_search->record)
#else
		if(!walminer_decode_context->reader_search->decoded_record)
#endif
		{
			wal2sql_search_end();
			return NULL;
		}

		walminer_search_record(walminer_decode_context->reader_search);
		walminer_debug("[search_wal] s_lsn=%x/%x",(uint32)(wdc->reader_search->ReadRecPtr >> 32), 
												  (uint32)wdc->reader_search->ReadRecPtr);

		
		if(0 != walminer_decode_context->swpro.decode_lsn && !wdc->swpro.get_decode_lsn &&
							walminer_decode_context->reader_search->ReadRecPtr >= wdc->swpro.decode_lsn)
		{
			/* search线程到达了decode_lsn */
			walminer_debug("[search_wal] get decode lsn %x/%x", 
										(uint32)(wdc->swpro.decode_lsn >> 32), 
										(uint32)wdc->swpro.decode_lsn);
			wdc->swpro.get_decode_lsn = true;
			wdc->swpro.decode_ckp_cell = list_tail(wdc->swpro.checkpoint_list);
#if (defined PG_VERSION_13)  || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
			wdc->swpro.timeline_id = wdc->reader_search->latestPageTLI;
			wdc->swpro.wal_seg_size = wdc->reader_search->segcxt.ws_segsize;
#elif (defined PG_VERSION_11) ||  (defined PG_VERSION_12)
			wdc->swpro.wal_seg_size = wdc->reader_search->wal_segment_size;
			wdc->swpro.timeline_id = wdc->reader_search->readPageTLI;
#else
			wdc->swpro.wal_seg_size = XLogSegSize;
			wdc->swpro.timeline_id = wdc->reader_search->readPageTLI;
#endif
		}

		if(0 != walminer_decode_context->swpro.end_lsn && !wdc->swpro.get_search_end &&
							walminer_decode_context->reader_search->ReadRecPtr >= walminer_decode_context->swpro.end_lsn)
		{
			/*search线程到达了end_lsn */
			walminer_debug("[search_wal] get end lsn");
			wdc->swpro.get_search_end = true;
			return NULL;
		}
    }
	return NULL;
}

static bool
wal2sql_wait_search(void)
{
		WalminerSearchWal		*swpro;			/*检索结果记录器*/

	swpro = &walminer_decode_context->swpro;
	/* 检查search_wal的结果，达到条件后修改decode的初始参数 */
	while(true)
	{
		if(swpro->get_decode_lsn)
		{
			/* 找到了开始decode的lsn */
			walminer_debug("[decode_wal]get decode lsn=%x/%x", 
									(uint32)(swpro->decode_lsn >> 32), (uint32)(swpro->decode_lsn));
			break;
		}
		lock_walminer_thread(3);
		if(swpro->get_search_end)
		{
			if(swpro->get_decode_lsn)
			{
				;/* 放弃处理继续循环 */
			}
			else
			{
				if(DECODE_TYPE_ALL == wal2sql_struct.at)
				{
					/* 不能到达代码 */
					elog(ERROR, "Why I can not get decode_lsn in DECODE_TYPE_ALL analyse type?");
				}
				else if(DECODE_TYPE_LSN ==  wal2sql_struct.at ||
						DECODE_TYPE_TIME == wal2sql_struct.at ||
						DECODE_TYPE_XID == wal2sql_struct.at)
				{
					/* 没有找到开始解析的点，解析结果为空 */
					return false;
				}
			}
		}
		unlock_walminer_thread(3);

		/* 等待找到decode_lsn */
		pg_usleep(1 * 1000000L);
	}
	return true;
}

static void
wal2sql_end(void)
{
	if(self_apply)
	{
		walminer_spi_finish();
	}
    self_apply = false;
    with_catalog = false;
    with_ddl = false;
}

static ListCell*
get_first_valid_ckp(void)
{
	List	 			*checkpoint_list = NULL;
	ListCell			*cell = NULL;
	ListCell			*result_cell = NULL;
	CheckpointInfo		*chpinfo = NULL;

	checkpoint_list = walminer_decode_context->swpro.checkpoint_list;
	Assert(checkpoint_list);

	foreach(cell, checkpoint_list)
	{
		chpinfo = lfirst(cell);
		if(walminer_decode_context->reader->ReadRecPtr <= chpinfo->chp_start_lsn)
		{
			result_cell = cell;
			break;
		}
	}
	return result_cell;
}

/*
 * 此函数是解析线程控制解析进度的函数,当解析线程获取到一个record之后，需要判断，当前是否
 * 应该解析这个record：1.解析;2.不解析;3.等待。
 * 
 * 1. 返回true解析这个record
 * 2. 返回false，标志着解析结束
 * 3. 搜索线程未结束的情况下，需要始终有下一个ckp供解析线程使用
 *    搜索线程结束的情况下就不需要等待了。
 *
 */
static bool
wal2sql_check_statue(void)
{
	XLogRecPtr			curlsn = 0;
	XLogRecPtr			end_lsn = 0;
	bool	 			get_search_end = false;
	List	 			*checkpoint_list = NULL;
	CheckpointInfo		*chpinfo = NULL;

	/* ------期望获取一个checkpoint点或者search线程结束 */
	while(!walminer_decode_context->anapro.ckp_valid)
	{
		ListCell 		*cell = NULL;
		ListCell		*nextcell = NULL;

		lock_walminer_thread(3);
		checkpoint_list = walminer_decode_context->swpro.checkpoint_list;
		get_search_end = walminer_decode_context->swpro.get_search_end;
		end_lsn = walminer_decode_context->swpro.end_lsn;
		unlock_walminer_thread(3);

		/* 不管检索线程是否结束，这里都需要检查是否可以找到下一个checkpoint点 */
		if(checkpoint_list)
		{
			if(!walminer_decode_context->anapro.ckp_cell)
			{
				/* 第一次判断ckp_cell的情况 */
				//walminer_decode_context->anapro.ckp_cell = list_head(checkpoint_list);
				walminer_decode_context->anapro.ckp_cell = get_first_valid_ckp();
				
				if(walminer_decode_context->anapro.ckp_cell)
				{
					chpinfo = lfirst(walminer_decode_context->anapro.ckp_cell);
					/*
					* 第一个checkpoint的redo点必须大于我们开始读的位置，而且大于我们已经读的位置才有效
					* 因为目前我们已经读取了一个record，这是防止第一个record就是checkpoint导致的奇怪问题
					*/
					if(chpinfo->chp_start_lsn >= walminer_decode_context->swpro.read_lsn)
					{
						walminer_debug("[check_search_statue] chp_start_lsn=%x/%x,swpro.read_lsn=%x/%x",
								(uint32)(chpinfo->chp_start_lsn >> 32), (uint32)(chpinfo->chp_start_lsn),
								(uint32)(walminer_decode_context->swpro.read_lsn >> 32), 
								(uint32)(walminer_decode_context->swpro.read_lsn));

						walminer_decode_context->anapro.ckp_valid = true;
						break;
					}
					else
					{
						/* 
						 * 未找到有效ckp,继续循环等待下一个checkpoint出现
						 * 或者等待search结束 
						 */
					}
				}
				else
				{
					/* 
					 * 未找到有效ckp,继续循环等待下一个checkpoint出现
					 * 或者等待search结束 
					 */
				}
			}
			else
			{
				cell = walminer_decode_context->anapro.ckp_cell;
#if (defined PG_VERSION_13)  || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
				nextcell = lnext(walminer_decode_context->swpro.checkpoint_list, cell);
#else
				nextcell = lnext(cell);
#endif
				if(nextcell)
				{
					walminer_debug("[check_search_statue] next cell");
					/* 找到了一个有效ckp, 跳出等待开始解析 */
					walminer_decode_context->anapro.ckp_cell = nextcell;
					walminer_decode_context->anapro.ckp_valid = true;

					break;
				}
			}
		}

		Assert(!walminer_decode_context->anapro.ckp_valid);
		if(get_search_end)
		{
			/* 在无有效ckp时，且search线程结束，那么不再等待有效ckp */
			if(checkpoint_list)
				walminer_debug("[check_search_statue] get_search_end ckp_list_length= %d", checkpoint_list->length);
			else
				walminer_debug("[check_search_statue] get_search_end ckp_list_length= 0");
			break;
		}
		/* 搜索线程没有找到checkpoint也没有结束,需要等一下搜索线程 */
		walminer_debug("[decode_wal]How search thread so slowly?");
		pg_usleep(1 * 1000000L);
	}

	Assert(walminer_decode_context->anapro.ckp_valid || get_search_end);
	curlsn = walminer_decode_context->reader->ReadRecPtr;

	/* -------------检查是否需要清理image */
	if(walminer_decode_context->anapro.ckp_valid)
	{
		chpinfo = lfirst(walminer_decode_context->anapro.ckp_cell);
		/* 下一个checkpoint有效,那么基于这个checkpoint检测是否清理image */
		walminer_debug("[check_search_statue]ReadRecPtr=%x/%x  vs chp_start_lsn=%x/%x",
				(uint32)(curlsn >> 32), (uint32)(curlsn),
				(uint32)(chpinfo->chp_start_lsn >> 32), (uint32)(chpinfo->chp_start_lsn));
		if(curlsn == chpinfo->chp_start_lsn)
		{
			walminer_debug("[check_search_statue] clean_image ckp_valid=false");
			clean_image();
			walminer_decode_context->anapro.ckp_valid = false;
		}
		else if(curlsn >= chpinfo->chp_start_lsn)
		{
			/* 
			 * 如果因为特殊原因，解析进程错过了一个checkpoint，这里跳过这个checkpoint
			 * 没有想好什么场景会出现这个情况，权当容错处理。
			 */
			walminer_debug("[check_search_statue] ckp_valid=false");
			walminer_decode_context->anapro.ckp_valid = false;
		}
		return true;
	}

	walminer_debug("[check_search_statue]end=%d,curlsn=%x/%x,end_lsn=%x/%x",get_search_end,
				(uint32)(curlsn >> 32), (uint32)(curlsn),
				(uint32)(end_lsn >> 32), (uint32)(end_lsn));
	if(get_search_end && curlsn >= end_lsn)
	{
		/* 已经到了search线程规定的结束位置 */
		return false;
	}
	else
	{
		Assert(curlsn < end_lsn);
		return true;
	}
}

/*
 * decode阶段的过滤
 */
static bool
wal2sql_filter_in_decode(RelFileNode *target_node, Oid reloid)
{
	/* 如果不需要解析系统表，且当前为系统表，则不解析这个record */
	if(!with_catalog && !with_ddl && FirstNormalObjectId > reloid)
	{
		return false;
	}

	/* 单表解析处理代码 */
	if(DECODE_MODE_SINGLE_TABLE == wal2sql_struct.am)
	{
		if(target_node->relNode != wal2sql_struct.relfilenode)
		{
			return false;
		}
	}
	return true;
}

Datum
wal2sql_with_catalog(PG_FUNCTION_ARGS)
{
	with_catalog = true;
	with_ddl = false;
	init_ddl_analyse();
	PG_RETURN_BOOL(with_catalog);
}

Datum
wal2sql_with_ddl(PG_FUNCTION_ARGS)
{
	with_ddl = true;
	with_catalog = false;
	init_ddl_analyse();
	PG_RETURN_BOOL(with_ddl);
}

Datum
wal2sql_self_apply(PG_FUNCTION_ARGS)
{
	self_apply = true;
	PG_RETURN_BOOL(self_apply);
}

/*
 * wal analyse begin here
 */
Datum
wal2sql_internal(PG_FUNCTION_ARGS)
{
	const char* result = "wal2sql success";

	if(regression_mode)
	{
		result = "pg_minerwal success";
	}
	check_all();

	memset(&wdecoder, 0, sizeof(WalminerDecode));
	set_call_back_funcs();
	init_wal2sql_struct();

	wal2sql_prepare();
	pg_walminer(fcinfo);
	wal2sql_end();
	PG_RETURN_TEXT_P(cstring_to_text(result));
}
