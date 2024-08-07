/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  walminer_decode.c
 *
 * 解析思路：
 * a. add的wal段全部解析
 * b. LSN范围指定
 * c. 时间范围指定
 * d. xid指定
 * 
 * a. 可能有事务不完整，可能有事务未提交，可能有数据不完整
 * b. 同0，但支持前探checkpoint模式
 * c. 从事务提交的wal记录中获取时间，时间入参是前开后闭区间。
 *    transaction1                          transaction2
 *        time1      ourtime_start/end        time2
 *    当我们找到了这样的wal组合，才认为找到了目标时间位置.
 *    对于开始时间，选择transaction1 commit record的end位置作为开始lsn
 *    对于结束时间，选择transaction2 commit record的end位置作为结束lsn(time2可以等于ourtime_end)
 *    这样就转化为1的解析方法。
 *    这个设计解析出的数据可能比实际数据偏多。
 * 
 * d. 这个是单xid解析，由于xid提交的顺序的不确定性，因此不再支持xid范围解析。
 *    查找到xid记录出现的位置和commit/abord的位置，根据RUNNING_XACT wal记录
 *    确定事务完整性。
 *   
 * 搜索线程+解析线程:
 * 	  搜索进程:从入参第一个wal开始检索，维护checkpoint链表，在解析模式b,c中还负责前探工作，
 * 	  在解析模式c,d中负责查找开始解析的LSN
 * 
 * 	  解析进程:在搜索进程确定好LSN(含开始维护image的LSN)后，开启解析过程。
 * 
 *-------------------------------------------------------------------------
 * 
 * 
 * 
 * 
 */
#include "datadictionary.h"
#include "wal2sql.h"
#include "wm_utils.h"
#include "replication/reorderbuffer.h"
#include "utils/memutils.h"
#include "access/heapam_xlog.h"
#include "catalog/pg_control.h"
#include "walminer_decode.h"
#ifdef PG_VERSION_9_5
#include "storage/standby.h"
#else
#include "storage/standbydefs.h"
#endif
#include <unistd.h>
#include "wm_compatible.h"

typedef struct TxDiskHead
{
	Size		total_size;
	bool		get_new;
	bool		get_old;
	int			new_len;
	int			old_len;
}TxDiskHead;
#if (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
static void walminer_decode_prune(WalRecordBuffer *buf);
static void walminer_decode_vacuum(WalRecordBuffer *buf);
#else
static void walminer_decode_clean(WalRecordBuffer *buf);
#endif
static void walminer_decode_mutiinsert(WalRecordBuffer *buf);
static void walminer_decode_insert(WalRecordBuffer *buf);
static void walminer_decode_delete(WalRecordBuffer *buf);
static void walminer_decode_update(WalRecordBuffer *buf);
static void reassemble_tuple_from_wal_data(char *data, Size len, ReorderBufferChange *change,
										TransactionId xmin, TransactionId xmax,  bool new);
static void walminer_decode_record(XLogReaderState *record);
static void walminer_decode_heap(WalRecordBuffer *buf);
static void walminer_decode_heap2(WalRecordBuffer *buf);
static void append_change_to_list(TransactionId xid, ReorderBufferChange *change);
static void walminer_decode_xact(WalRecordBuffer *buf);
static void walminer_decode_abort(TransactionId xid);
static void reassemble_tuple_from_heap_tuple_header(HeapTupleHeader hth,
					Size len, ReorderBufferTupleBuf *tuple);
static bool
reassemble_tuplenew_from_wal_data(char *data, Size len, ReorderBufferChange *change, xl_heap_update *xlrec,
							BlockNumber blknum_new, BlockNumber blknum_old, TransactionId xid);
static void free_transaction_entry(TransactionEntry *te);
static void free_sql_record(TransactionEntry *te);
static void heap_page_prune_execute_walminer(Page page,
						OffsetNumber *redirected, int nredirected,
						OffsetNumber *nowdead, int ndead,
						OffsetNumber *nowunused, int nunused);

WalmierDecodingContext  *walminer_decode_context = NULL;


static void
heap_page_prune_execute_walminer(Page page,
						OffsetNumber *redirected, int nredirected,
						OffsetNumber *nowdead, int ndead,
						OffsetNumber *nowunused, int nunused)
{
	OffsetNumber *offnum;
	int			i;

	/* Update all redirected line pointers */
	offnum = redirected;
	for (i = 0; i < nredirected; i++)
	{
		OffsetNumber fromoff = *offnum++;
		OffsetNumber tooff = *offnum++;
		ItemId		fromlp = PageGetItemId(page, fromoff);

		ItemIdSetRedirect(fromlp, tooff);
	}

	/* Update all now-dead line pointers */
	offnum = nowdead;
	for (i = 0; i < ndead; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

		ItemIdSetDead(lp);
	}

	/* Update all now-unused line pointers */
	offnum = nowunused;
	for (i = 0; i < nunused; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

		ItemIdSetUnused(lp);
	}

	/*
	 * Finally, repair any fragmentation, and update the page's hint bit about
	 * whether it has free pointers.
	 */
	PageRepairFragmentation(page);
}

/*创建WalmierDecodingContext结构*/
void
create_walminer_decode_context(void)
{
	MemoryContext 			context, old_context;

	context = AllocSetContextCreate(CurrentMemoryContext,
									"Walminer context",
									ALLOCSET_DEFAULT_SIZES);
	MemoryContextAllowInCriticalSection(context, true);
	old_context = MemoryContextSwitchTo(context);
	walminer_decode_context = (WalmierDecodingContext*)walminer_malloc(sizeof(WalmierDecodingContext), 0);

	walminer_decode_context->context = context;
	walminer_decode_context->dict_dboid = wdd.ddh.dboid;
	init_thread_info();
	MemoryContextSwitchTo(old_context);

}

static void
free_sql_record(TransactionEntry *te)
{
	SQLRecord *sqlr = NULL;

	Assert(te);
	sqlr = &te->sql;
	walminer_free((char*)sqlr->notsql.data, 0);
	walminer_free((char*)sqlr->sql_pro.data, 0);
	walminer_free((char*)sqlr->sql_undo.data, 0);
	walminer_free((char*)sqlr->sql_temp_1.data, 0);
	walminer_free((char*)sqlr->sql_temp_2.data, 0);
}

void
free_change_list(List *change_list)
{
	ListCell *cell = NULL;
	foreach(cell, change_list)
	{
		ReorderBufferChange *change = NULL;

		change = lfirst(cell);
		if(REORDER_BUFFER_CHANGE_INSERT == change->action
			|| REORDER_BUFFER_CHANGE_DELETE == change->action
			|| REORDER_BUFFER_CHANGE_UPDATE == change->action
			|| REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT == change->action)
		{
			if(change->data.tp.newtuple)
				walminer_free((char*)change->data.tp.newtuple, 0);
			if(change->data.tp.oldtuple)
				walminer_free((char*)change->data.tp.oldtuple, 0);
		}
	}
	list_free_deep(change_list);
}

static void
free_transaction_entry(TransactionEntry *te)
{
	/* 清理toast数据 */
	if(te->toast_list)
		list_free_deep(te->toast_list);
	
	/* 清理changes数据 */
	if(te->changes)
	{
		free_change_list(te->changes);
		te->changes = NULL;
	}
	walminer_free((char*)te, 0);
}

void
remove_transaction_entry_from_list(TransactionEntry *te)
{
	WalmierDecodingContext 	*wdc = walminer_decode_context;
	char			transaction_file_path[MAXPGPATH] = {0};
	char			t_file_name[NAMEDATALEN] = {0};

	Assert(te);
	free_sql_record(te);
	walminer_debug("[remove_transaction_entry_from_list]xid=%u", te->xid);
	
	walminer_debug("[remove_transaction_entry_from_list] list-length==%d",  wdc->anapro.xactlist->length);
	wdc->anapro.xactlist = list_delete_ptr(wdc->anapro.xactlist, (char*)te);
	if( wdc->anapro.xactlist)
		walminer_debug("[remove_transaction_entry_from_list] list-length==%d",  wdc->anapro.xactlist->length);
	else
		walminer_debug("[remove_transaction_entry_from_list] list-length==0");

	if(wdc->anapro.current_xid == te->xid)
	{
		wdc->anapro.current_entry = NULL;
		wdc->anapro.current_xid = InvalidTransactionId;
	}

	sprintf(t_file_name, "xid_%d.spill", te->xid);
	get_transaction_pach(transaction_file_path, t_file_name);
	if(is_file_exist(transaction_file_path))
	{
		remove(transaction_file_path);
	}

	free_transaction_entry(te);
}

/*
 * 维护事务链表的函数，TODO(lchch)此处应该改为哈希表实现
 * 
 * origlsn为0时，认为这时一次单纯的查找
 * 不为0且没有找到目标时需要向list中插入一个entry
 * 
 */
TransactionEntry*
get_transaction_from_list(TransactionId xid, XLogRecPtr origlsn)
{
	List 		*xact_list = NULL;
	ListCell	*cell = NULL;

	TransactionEntry *te = NULL;
	WalmierDecodingContext *wdc = walminer_decode_context;

	walminer_debug("[get_transaction_from_list]xid=%d, olsn=%x/%x", xid, (uint32)(origlsn >> 32), (uint32)origlsn);

	/*快速查找*/
	if(wdc->anapro.current_xid == xid)
	{
		walminer_debug("[get_transaction_from_list]FIND ENTRY1 xid=%d", xid);
		return wdc->anapro.current_entry;
	}

	xact_list = wdc->anapro.xactlist;

	foreach(cell, xact_list)
	{
		TransactionEntry *te_loop = NULL;

		te_loop = (TransactionEntry*)lfirst(cell);
		if(te_loop->xid == xid)
		{
			te = te_loop;
			break;
		}
	}

	/*
	 * origlsn为0时，认为这时一次单纯的查找
	 * 不为0且没有找到目标时需要向list中插入一个entry
	 */
	if(!te && 0 != origlsn)
	{
		te = (TransactionEntry*)walminer_malloc(sizeof(TransactionEntry), 0);
		te->xid = xid;
		te->first_lsn = origlsn;

		/* 判断事务是否为完整 */
		if(0 == walminer_decode_context->swpro.first_sure_xid)
		{
			te->complete = false;
		}
		else
		{
			if(TransactionIdPrecedes(xid, walminer_decode_context->swpro.first_sure_xid))
			{
				te->complete = false;
			}
			else
			{
				te->complete = true;
			}
		}
		wdc->anapro.xactlist = lappend(wdc->anapro.xactlist, te);
		walminer_debug("[get_transaction_from_list] INSERT ENTRY xid=%d, olsn=%x/%x", xid, (uint32)(origlsn >> 32), (uint32)origlsn);
	}

	/*更新快速查找信息*/
	if(te)
	{
		wdc->anapro.current_xid = xid;
		wdc->anapro.current_entry = te;
		walminer_debug("[get_transaction_from_list]FIND ENTRY2 xid=%d", xid);
	}
	
	return te;
}

static void
append_change_to_list(TransactionId xid, ReorderBufferChange *change)
{
	List				*change_list = NULL;
	TransactionEntry 	*te = NULL;

	te = get_transaction_from_list(xid, InvalidXLogRecPtr);
	change_list = te->changes;

	te->changes = lappend(change_list, (void*)change);
	walminer_debug("[append_change_to_list] xid=%u,kind=%d", xid,change->action);
	te->nentries_mem++;
	te->nentries++;

	big_transaction_spill(te, false);
}

static void
walminer_decode_record(XLogReaderState *record)
{
	WalRecordBuffer	buf;
	WalmierDecodingContext *wdc = walminer_decode_context;
	uint8			cur_rmgrid = 0;

	memset(&buf, 0, sizeof(WalRecordBuffer));
	buf.record = record;
	buf.endptr = wdc->reader->ReadRecPtr;
	buf.origptr = wdc->reader->EndRecPtr;

	record_store_image(record);

	/* cast so we get a warning when new rmgrs are added */
	cur_rmgrid = XLogRecGetRmid(record);
	walminer_debug("[walminer_decode_record]cur_rmgrid=%d", cur_rmgrid);
	switch ((RmgrIds) cur_rmgrid)
	{
		/*
		 * Rmgrs we care about for logical decoding. Add new rmgrs in
		 * rmgrlist.h's order.
		 */
		case RM_XACT_ID:
			walminer_decode_xact(&buf);
			break;

		case RM_HEAP_ID:
			walminer_decode_heap(&buf);
			break;

		case RM_STANDBY_ID:
			//DecodeStandbyOp(&buf);
			break;
		
		case RM_HEAP2_ID:
			walminer_decode_heap2(&buf);
			break;
#ifndef	PG_VERSION_9_5
		case RM_LOGICALMSG_ID:
			//DecodeLogicalMsgOp(&buf);
			break;
		case RM_GENERIC_ID:
			/* just deal with xid, and done */
			//ReorderBufferProcessXid(wdc->reorder, XLogRecGetXid(record),
			//						buf.origptr);
			break;
#endif
		
		case RM_XLOG_ID:
			//DecodeXLogOp(&buf);

			/*
			 * Rmgrs irrelevant for logical decoding; they describe stuff not
			 * represented in logical decoding. Add new rmgrs in rmgrlist.h's
			 * order.
			 */
			break;
		case RM_SMGR_ID:
		case RM_CLOG_ID:
		case RM_DBASE_ID:
		case RM_TBLSPC_ID:
		case RM_MULTIXACT_ID:
		case RM_RELMAP_ID:
		case RM_BTREE_ID:
		case RM_HASH_ID:
		case RM_GIN_ID:
		case RM_GIST_ID:
		case RM_SEQ_ID:
		case RM_SPGIST_ID:
		case RM_BRIN_ID:
		case RM_COMMIT_TS_ID:
		case RM_REPLORIGIN_ID:
			break;
		case RM_NEXT_ID:
			elog(ERROR, "unexpected RM_NEXT_ID rmgr_id: %u", (RmgrIds) XLogRecGetRmid(buf.record));
	}
}

static void
walminer_decode_xact(WalRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	uint8		info = XLogRecGetInfo(r) & XLOG_XACT_OPMASK;
	
	walminer_debug("[walminer_decode_xact]info=%x", info);

	switch (info)
	{
		case XLOG_XACT_COMMIT:
		case XLOG_XACT_COMMIT_PREPARED:
		{
			xl_xact_commit *xlrec;
			xl_xact_parsed_commit parsed;
			TransactionId xid;

			xlrec = (xl_xact_commit *) XLogRecGetData(r);
			ParseCommitRecord(XLogRecGetInfo(r), xlrec, &parsed);
			if(XLOG_XACT_COMMIT_PREPARED == info)
				xid = parsed.twophase_xid;
			else
				xid = XLogRecGetXid(r);
			walminer_debug("[walminer_decode_xact] xid=%u", xid);
			if(wdecoder.w_call_funcs.walminer_decode_commit)
				wdecoder.w_call_funcs.walminer_decode_commit(buf, &parsed, xid);
			break;
		}
		case XLOG_XACT_ABORT:
		case XLOG_XACT_ABORT_PREPARED:
		{
			TransactionId xid;
			xl_xact_abort *xlrec;
			xl_xact_parsed_abort parsed;

			xlrec = (xl_xact_abort *) XLogRecGetData(r);
			ParseAbortRecord(XLogRecGetInfo(r), xlrec, &parsed);
			if(XLOG_XACT_ABORT_PREPARED == info)
				xid = parsed.twophase_xid;
			else
				xid = XLogRecGetXid(r);

			walminer_decode_abort(xid);
			break;
		}
		case XLOG_XACT_ASSIGNMENT:
			break;
		
		case XLOG_XACT_PREPARE:
#if (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
		case XLOG_XACT_INVALIDATIONS:
#endif
			/* 对解析过程无意义的wal记录无意义 */
			break;
		default:
			elog(ERROR, "unexpected RM_XACT_ID record type: %u", info);
	}
}

static void
walminer_decode_abort(TransactionId xid)
{
	TransactionEntry 	*te = NULL;

	te = get_transaction_from_list(xid, InvalidXLogRecPtr);
	if (te == NULL)
		return;
	walminer_debug("[walminer_decode_abort]xid=%u", xid);
	remove_transaction_entry_from_list(te);
	return;
}

static void
walminer_decode_heap2(WalRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_HEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);

	walminer_debug("[walminer_decode_heap2]info=%x", info);

	if(InvalidTransactionId != xid)
		get_transaction_from_list(xid, buf->origptr);

	switch (info)
	{
		case XLOG_HEAP2_MULTI_INSERT:
			walminer_decode_mutiinsert(buf);
			break;
		case XLOG_HEAP2_NEW_CID:
			/* 系统表使用，而且这个信息对walminer无意义 */
			break;
		case XLOG_HEAP2_REWRITE:
			/* 
			 * 这个wal类型在逻辑解析过程中，记录关系relfilenode变化的历程
			 * 目前walmienr不需要这个信息
			 */
			break;
		case XLOG_HEAP2_FREEZE_PAGE:
			/*
			 * 对一个page中的元组进行freeze操作，
			 * 目前walminer不需要这个信息
			 */
			break;
#if (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
		case XLOG_HEAP2_VACUUM:
			walminer_decode_vacuum(buf);
			break;
		case XLOG_HEAP2_PRUNE:
			walminer_decode_prune(buf);
			break;
#else
		case XLOG_HEAP2_CLEAN:
			walminer_decode_clean(buf);
			break;
		case XLOG_HEAP2_CLEANUP_INFO:
			/*
			 * 主备交互使用的wal类型，walminer不需要这个信息
			 */
			break;
#endif
		case XLOG_HEAP2_VISIBLE:
			/*
			 * 标记一个page全部可见，walminer不需要这个信息
			 */
			break;
		case XLOG_HEAP2_LOCK_UPDATED:
			/*
			 * hot链上的tuple锁定，walminer不需要这个信息
			 */
			break;
		default:
			elog(ERROR, "unexpected RM_HEAP2_ID record type: %u", info);
	}
}

static void
walminer_decode_heap(WalRecordBuffer *buf)
{
	WalmierDecodingContext 	*wdc = walminer_decode_context;
	uint8					info = XLogRecGetInfo(wdc->reader) & XLOG_HEAP_OPMASK;
	TransactionId 			xid = XLogRecGetXid(wdc->reader);
	
	walminer_debug("[walminer_decode_heap]info=%x", info);

	if(InvalidTransactionId != xid)
		get_transaction_from_list(xid, buf->origptr);

	switch (info)
	{
		case XLOG_HEAP_INSERT:
			walminer_decode_insert(buf);
			break;
		case XLOG_HEAP_HOT_UPDATE:
		case XLOG_HEAP_UPDATE:
			walminer_decode_update(buf);
			break;

		case XLOG_HEAP_DELETE:
			walminer_decode_delete(buf);
			break;
#ifndef PG_VERSION_10
		case XLOG_HEAP_TRUNCATE:
			/* 
			 * 此处记录了truncate表的oid,truncate可能会导致reloid->relfilenode映射改变，
			 * 会产生数据字典有数据不匹配的情况(限制项1)。
			 * 
			 * Truncate在walminer中认为是DDL语句，当前walminer不解析DDL语句
			 * TODO(lchch):Truncate DDL解析
			 */
			//DecodeTruncate(wdc, buf);
			break;
#endif
		case XLOG_HEAP_INPLACE:
			/*
			 * 在数据库初始化/pg_class更新统计信息/pg_database更新frozenxid时会出现这个wal
			 * 记录，这个记录对walminer解析不产生影响，目前也不关心这个信息。
			 */
			break;

		case XLOG_HEAP_CONFIRM:
			/*
			 * 预测插入(do nothing)
			 * 正常没有冲突的情况下，会有一个预测insert+heap_confirm wal记录
			 * 如果有指定列冲突，则不会产生wal日志
			 * 如果产生并行冲突，会有预测insert+delete
			 * 结论：正常解析insert + delete即可。（未找到测试用例）
			 * 
			 * 
			 * 预测插入(do update)
			 * 正常没有冲突的情况下，会有一个预测insert+heap_confirm wal记录
			 * 如果有指定列冲突，会有一个update记录,如果update后还有冲突则有一个abort
			 * 结论：正常解析update即可。
			 * 
			 * 最终结论:如果不寻求最优方式，可以忽略这个wal类型。
			 */
			break;

		case XLOG_HEAP_LOCK:
			/*
			 * 加行锁，walminer解析过程不需要这个信息
			 */
			break;

		default:
			elog(ERROR, "unexpected RM_HEAP_ID record type: %u", info);
			break;
	}
}

/*
 * 在有全页写时，使用这个函数合成tuple
 */
static void
reassemble_tuple_from_heap_tuple_header(HeapTupleHeader hth,  Size len, ReorderBufferTupleBuf *tuple)
{
	HeapTupleHeader header = NULL;
	/*char *pp = NULL;
	int loop = 0;*/

	tuple->tuple.t_len = len;
	header = tuple->tuple.t_data;

	ItemPointerSetInvalid(&tuple->tuple.t_self);
	tuple->tuple.t_tableOid = InvalidOid;

	walminer_debug("[reassemble_tuple]len=%ld", len);

	memcpy(header, hth, len);
}

static void
reassemble_tuple_from_wal_data(char *data, Size len, ReorderBufferChange *change,
			TransactionId xmin, TransactionId xmax, bool new)
{
	xl_heap_header xlhdr;
	int			datalen = len - SizeOfHeapHeader;
	HeapTupleHeader header;
	ReorderBufferTupleBuf *tuple = NULL;

	if(new)
		tuple = change->data.tp.newtuple;
	else
		tuple = change->data.tp.oldtuple;
	Assert(datalen >= 0);

	tuple->tuple.t_len = datalen + SizeofHeapTupleHeader;
	header = tuple->tuple.t_data;

	/* not a disk based tuple */
	ItemPointerSetInvalid(&tuple->tuple.t_self);

	/* we can only figure this out after reassembling the transactions */
	tuple->tuple.t_tableOid = InvalidOid;

	/* data is not stored aligned, copy to aligned storage */
	memcpy((char *) &xlhdr,
		   data,
		   SizeOfHeapHeader);

	memset(header, 0, SizeofHeapTupleHeader);

	memcpy(((char *) tuple->tuple.t_data) + SizeofHeapTupleHeader,
		   data + SizeOfHeapHeader,
		   datalen);

	header->t_infomask = xlhdr.t_infomask;
	header->t_infomask2 = xlhdr.t_infomask2;
	header->t_hoff = xlhdr.t_hoff;
	if(InvalidTransactionId != xmin)
		HeapTupleHeaderSetXmin(header, xmin);
	if(InvalidTransactionId != xmax)
		HeapTupleHeaderSetXmax(header, xmax);
}

static void
reassemble_mutituple_from_wal_data(char *data, Size len, ReorderBufferChange *change,
										xl_multi_insert_tuple *xlhdr)
{
	HeapTupleHeader header;
	ReorderBufferTupleBuf *tuple = NULL;

	tuple = change->data.tp.newtuple;
	header = tuple->tuple.t_data;
	ItemPointerSetInvalid(&tuple->tuple.t_self);
	tuple->tuple.t_tableOid = InvalidOid;
	tuple->tuple.t_len = len + SizeofHeapTupleHeader;

	memset(header, 0, SizeofHeapTupleHeader);

	memcpy(((char *) tuple->tuple.t_data) + SizeofHeapTupleHeader,
		   data,
		   len);

	header->t_infomask = xlhdr->t_infomask;
	header->t_infomask2 = xlhdr->t_infomask2;
	header->t_hoff = xlhdr->t_hoff;
}


/*
 * 组装update的新值，
 * 因为update新值有suffixlen和prefixlen拼接，还有可能会从旧page获取数据，所以单拿出来
 */
static bool
reassemble_tuplenew_from_wal_data(char *data, Size len, ReorderBufferChange *change, xl_heap_update *xlrec,
							BlockNumber blknum_new, BlockNumber blknum_old, TransactionId xid)
{
	//change->data.tp.newtuple
	xl_heap_header 			xlhdr;
	HeapTupleHeader			htup;					/* 组装新tuple时，便于coding的指针 */
	HeapTupleHeader			htup_old= NULL;		/* 用于获取旧的tuple，然后从中拷贝重组新tuple所需要的数据 */
	ReorderBufferTupleBuf	*recorbuff_new = NULL;
	char					*newp = NULL;
	char			page_old[BLCKSZ] = {0};
	int				pageindex_old = 0;
	char			*recdata, *recdata_end;
	int				suffixlen = 0;
	int				prefixlen = 0;
	int				tuplen = 0; /* 记录update wal record中用户数据的长度 */
	int				old_tuple_len = 0;
	int				reass_tuple_len = 0;
	ItemPointerData	target_tid;
	bool			get_old_page = false;
	
	/*
	 * 在有prefixlen或suffixlen值的情况下,需要去获取oldpage然后取得oldtuple。
	 * 然后使用wal日志中的值覆盖更新值。
	 */
	if((xlrec->flags & XLH_UPDATE_PREFIX_FROM_OLD) || (xlrec->flags & XLH_UPDATE_SUFFIX_FROM_OLD))
	{
		ItemId	id = NULL;

		walminer_debug("[reassemble_tuplenew_from_wal_data] have prefix or suffix");
		get_old_page= get_image_from_store(GET_CHANGE_RELNODE(change), MAIN_FORKNUM, blknum_old,
																	(char*)page_old, &pageindex_old);
		walminer_debug("[reassemble_tuplenew_from_wal_data] get_old_page=%d", get_old_page);
		/* 如果没有找到旧的page，那么就不解析这个update了 */															
		if(!get_old_page)
			return false;
		id = PageGetItemId(page_old, xlrec->old_offnum);
		old_tuple_len = (Size)id->lp_len;
		htup_old = (HeapTupleHeader)PageGetItem(page_old, id);
	}
	recdata = data;
	recdata_end = data + len;

	if (xlrec->flags & XLH_UPDATE_PREFIX_FROM_OLD)
	{
		Assert(blknum_old == blknum_new);
		memcpy(&prefixlen, recdata, sizeof(uint16));
		recdata += sizeof(uint16);
	}
	if (xlrec->flags & XLH_UPDATE_SUFFIX_FROM_OLD)
	{
		Assert(blknum_old == blknum_old);
		memcpy(&suffixlen, recdata, sizeof(uint16));
		recdata += sizeof(uint16);
	}
	memcpy((char *) &xlhdr, recdata, SizeOfHeapHeader);
	recdata += SizeOfHeapHeader;

	tuplen = recdata_end - recdata;
	reass_tuple_len = SizeofHeapTupleHeader + tuplen + prefixlen + suffixlen;

	change->data.tp.newtuple = get_tuple_space(tuplen + prefixlen + suffixlen);
	recorbuff_new = change->data.tp.newtuple;
	recorbuff_new->tuple.t_len = reass_tuple_len;

	recorbuff_new = change->data.tp.newtuple;
	htup = recorbuff_new->tuple.t_data;

	ItemPointerSetInvalid(&recorbuff_new->tuple.t_self);
	ItemPointerSet(&target_tid, blknum_new, xlrec->new_offnum);

	/* we can only figure this out after reassembling the transactions */
	recorbuff_new->tuple.t_tableOid = InvalidOid;

	htup = (HeapTupleHeader)recorbuff_new->tuple.t_data;

	newp = (char *) htup + SizeofHeapTupleHeader;

	if (prefixlen > 0)
	{
		int 		len;

		/* 拷贝实际数据前的填充部分 */
		len = xlhdr.t_hoff - SizeofHeapTupleHeader;
		memcpy(newp, recdata, len);
		recdata += len;
		newp += len;

		Assert(htup_old);
		/* copy prefix from old tuple */
		memcpy(newp, (char *) htup_old + htup_old->t_hoff, prefixlen);
		newp += prefixlen;

		/* copy new tuple data from WAL record */
		len = tuplen - (xlhdr.t_hoff - SizeofHeapTupleHeader);
		memcpy(newp, recdata, len);
		recdata += len;
		newp += len;
	}
	else
	{
		/*
		 * copy bitmap [+ padding] [+ oid] + data from record
		 */
		memcpy(newp, recdata, tuplen);
		recdata += tuplen;
		newp += tuplen;
	}
	Assert(recdata == recdata_end);

	/* copy suffix from old tuple */
	if (suffixlen > 0)
	{
		Assert(htup_old);
		memcpy(newp, (char *) htup_old + old_tuple_len - suffixlen, suffixlen);
	}

	htup->t_infomask2 = xlhdr.t_infomask2;
	htup->t_infomask = xlhdr.t_infomask;
	htup->t_hoff = xlhdr.t_hoff;
	HeapTupleHeaderSetXmin(htup, xid);
	HeapTupleHeaderSetCmin(htup, FirstCommandId);
	HeapTupleHeaderSetXmax(htup, xlrec->new_xmax);
	/* Make sure there is no forward chain link in t_ctid */
	htup->t_ctid = target_tid;

	return true;
}

#if (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
static void
walminer_decode_vacuum(WalRecordBuffer *buf)
{
	RelFileNode 			rnode;
	BlockNumber 			blkno = 0;
	xl_heap_vacuum 			*xlrec = NULL;

	XLogReaderState 		*record = NULL;
	int						pageindex = 0;
	bool					get_image = false;
	char					page[BLCKSZ] = {0};
	XLogRecPtr				lsn = 0;

	memset(&rnode, 0, sizeof(RelFileNode));
	record = buf->record;
	lsn = record->EndRecPtr;

	xlrec = (xl_heap_vacuum *) XLogRecGetData(record);

	wm_XLogRecGetBlockTag(record, 0, &rnode, NULL, &blkno);
	if (!filter_in_decode(&rnode))
		return;

	get_image = XLogRecHasBlockImage(record, 0);
	if(!get_image)
	{
		OffsetNumber			*nowunused = NULL;
		Size					datalen = 0;
		OffsetNumber			*offnum;

		nowunused = (OffsetNumber *) XLogRecGetBlockData(record, 0, &datalen);
		offnum = nowunused;

		if(!get_image_from_store(&rnode, MAIN_FORKNUM, blkno, page, &pageindex))
		{
			return;
		}

		for (int i = 0; i < xlrec->nunused; i++)
		{
			OffsetNumber off = *offnum++;
			ItemId		lp = PageGetItemId(page, off);

			Assert(ItemIdIsDead(lp) && !ItemIdHasStorage(lp));
			ItemIdSetUnused(lp);
		}

		/* Attempt to truncate line pointer array now */
		PageTruncateLinePointerArray(page);

		PageSetLSN(page, lsn);

		walminer_debug("[walminer_decode_vacuum]blkno=%u rnode=%u", blkno, rnode.relNode);


		flush_page(pageindex, page);
	}
}

#endif

static void
#if (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
walminer_decode_prune(WalRecordBuffer *buf)
#else
walminer_decode_clean(WalRecordBuffer *buf)
#endif
{
	RelFileNode 			rnode;
	BlockNumber 			blkno = 0;
#if (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
	xl_heap_prune 			*xlrec = NULL;
#else
	xl_heap_clean 			*xlrec = NULL;
#endif
	XLogReaderState 		*record = NULL;
	int						pageindex = 0;
	bool					get_image = false;
	char					page[BLCKSZ] = {0};

	memset(&rnode, 0, sizeof(RelFileNode));
	record = buf->record;

#if (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
	xlrec = (xl_heap_prune *) XLogRecGetData(record);
#else
	xlrec = (xl_heap_clean *) XLogRecGetData(record);
#endif

	wm_XLogRecGetBlockTag(record, 0, &rnode, NULL, &blkno);
	if (!filter_in_decode(&rnode))
		return;

	get_image = XLogRecHasBlockImage(record, 0);
	if(!get_image)
	{
		OffsetNumber 			*end = NULL;
		OffsetNumber 			*redirected = NULL;
		OffsetNumber 			*nowdead = NULL;
		OffsetNumber			*nowunused = NULL;
		int						nredirected = 0;
		int						ndead = 0;
		int						nunused = 0;
		Size					datalen = 0;

		if(!get_image_from_store(&rnode, MAIN_FORKNUM, blkno, page, &pageindex))
		{
			/* 
			 * 因为没有找到初始checkpoint，可能会导致此处找不到image，
			 * 此wal record仅仅为了redo page变化，没有数据产出，所以此处可以
			 * 没有任何提示
			 */
			return;
		}
		redirected = (OffsetNumber *) XLogRecGetBlockData(record, 0, &datalen);
		nredirected = xlrec->nredirected;
		ndead = xlrec->ndead;
		end = (OffsetNumber *) ((char *) redirected + datalen);
		nowdead = redirected + (nredirected * 2);
		nowunused = nowdead + ndead;
		nunused = (end - nowunused);
		Assert(nunused >= 0);

		walminer_debug("[walminer_decode_clean]blkno=%u rnode=%u", blkno, rnode.relNode);
		heap_page_prune_execute_walminer(page,
								redirected, nredirected,
								nowdead, ndead,
								nowunused, nunused);

		flush_page(pageindex, page);
	}
}

static void
walminer_decode_mutiinsert(WalRecordBuffer *buf)
{
	XLogReaderState 		*r = buf->record;
	xl_heap_multi_insert 	*xlrec;
	int						i;
	char	   				*data;
	char	   				*tupledata;
	Size					tuplelen;
	RelFileNode 			rnode;
	bool					has_image = false;
	int						pageindex = 0;
	bool					isinit = false;
	BlockNumber				blknum = InvalidBlockNumber;
	TransactionId			xid = InvalidTransactionId;
	char					page[BLCKSZ] = {0};
	

	xlrec = (xl_heap_multi_insert *) XLogRecGetData(r);
	wm_XLogRecGetBlockTag(r, 0, &rnode, NULL, &blknum);
	if (!filter_in_decode(&rnode))
		return;
	
	tupledata = XLogRecGetBlockData(r, 0, &tuplelen);
	data = tupledata;
	xid = XLogRecGetXid(r);

	has_image = XLogRecHasBlockImage(r, 0);
	if(has_image)
	{
		if(!get_image_from_store(&rnode, MAIN_FORKNUM, blknum, (char*)page, &pageindex))
		{
			/*不应该到达的代码*/
			elog(ERROR,"[mutiinsert]can not find imagestore about reloid=%u,blckno=%u", rnode.relNode, blknum);
		}
	}

	if(XLOG_HEAP_INIT_PAGE & XLogRecGetInfo(r))
	{
		isinit = true;
	}
	
	
	for (i = 0; i < xlrec->ntuples; i++)
	{
		ReorderBufferChange 	*change = NULL;
		xl_multi_insert_tuple 	*xlhdr = NULL;
		int						datalen = 0;
		OffsetNumber			offnum = InvalidOffsetNumber;

		if(isinit)
			offnum = i + FirstOffsetNumber;
		else
			offnum = xlrec->offsets[i];
		walminer_debug("[walminer_decode_mutiinsert]i=%d, offnum=%d", i, offnum);
		change = get_change_space();
		change->action = REORDER_BUFFER_CHANGE_INSERT;
		change->origin_id = XLogRecGetOrigin(r);
		memcpy(GET_CHANGE_RELNODE(change), &rnode, sizeof(RelFileNode));

		if (has_image)
		{
			
			ItemId			id = NULL;
			HeapTupleHeader	tuphdr = NULL;
			int				current_tup_len = 0;

			walminer_debug("[walminer_decode_mutiinsert]has_image offnum=%d", offnum);
			id = PageGetItemId(page, offnum);
			tuphdr = (HeapTupleHeader)PageGetItem(page, id);
			current_tup_len = (Size)id->lp_len;

			change->data.tp.newtuple = get_tuple_space(current_tup_len);
			
			reassemble_tuple_from_heap_tuple_header(tuphdr, current_tup_len, change->data.tp.newtuple);
		}
		else
		{
			if(isinit && FirstOffsetNumber == offnum)
			{
				page_init_by_xlog(&rnode, MAIN_FORKNUM, blknum);
			}
			Assert(tupledata);
			xlhdr = (xl_multi_insert_tuple *) SHORTALIGN(data);
			data = ((char *) xlhdr) + SizeOfMultiInsertTuple;
			datalen = xlhdr->datalen;

			walminer_debug("[walminer_decode_mutiinsert]has data");
			change->data.tp.newtuple = get_tuple_space(datalen);
			reassemble_mutituple_from_wal_data(data, datalen, change, xlhdr);
			data += datalen;

			/* 
			* 此处为向image redo的代码
			* 
			* 如果找到了前面的全页写数据且无FPI,应该更新我们记录的image信息
			* 如果没有找到，说明没有找到开始的checkpoint信息,此处不再多作处理
			*/
			if(get_image_from_store(&rnode, MAIN_FORKNUM, blknum, (char*)page, &pageindex))
			{
				HeapTupleHeader 		htup = NULL;
				Size					newlen = 0;

				walminer_debug("[walminer_decode_mutiinsert] redo to image");
				htup = change->data.tp.newtuple->tuple.t_data;
				newlen =  change->data.tp.newtuple->tuple.t_len;
				if(InvalidOffsetNumber == PageAddItem(page, (Item) htup, newlen, offnum, true, true))
					elog(ERROR, "failed to add tuple:offsetnum %d,relnode %u, blckno %d", offnum, rnode.relNode, blknum);
				flush_page(pageindex, page);
				//out_page_to_file(page, target_node.relNode, blknum, 2);
			}
			else
			{
				if(!regression_mode)
					elog(NOTICE, "Can not find FPW for relnode %u, blckno %d, ignore redo.",
															rnode.relNode, blknum);
			}
		}

		change->data.tp.clear_toast_afterwards = true;

		append_change_to_list(xid, change);
	}
}



/*
 * 将wal记录decode为change格式
 * 若为FPW数据,从image中读取数据，填充tuple
 * 若不为FPW数据，将wal记录中的数据redo到Image中
 */
static void
walminer_decode_insert(WalRecordBuffer *buf)
{
	Size				datalen = 0;
	char	   			*tupledata = NULL;
	Size				tuplelen = 0;
	xl_heap_insert 		*xlrec = NULL;
	ReorderBufferChange *change;
	TransactionId 		xid = 0;
	RelFileNode 		target_node;
	XLogReaderState 	*r = buf->record;
	ForkNumber 			forknum = InvalidForkNumber;
	BlockNumber 		blknum = InvalidBlockNumber;
	int					pageindex = 0;
	char				page[BLCKSZ] = {0};
	WalmierDecodingContext *wdc = walminer_decode_context;

	memset(&target_node, 0, sizeof(RelFileNode));

	xlrec = (xl_heap_insert *) XLogRecGetData(r);

	/* 不关心其他数据库 */
	wm_XLogRecGetBlockTag(r, 0, &target_node, &forknum, &blknum);
	if (!filter_in_decode(&target_node))
		return;

	change = get_change_space();
	if (!(xlrec->flags & XLH_INSERT_IS_SPECULATIVE))
		change->action = REORDER_BUFFER_CHANGE_INSERT;
	else
		change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT;
	change->origin_id = XLogRecGetOrigin(r);
	memcpy(GET_CHANGE_RELNODE(change), &target_node, sizeof(RelFileNode));
	change->lsn = r->ReadRecPtr;
	xid = XLogRecGetXid(r);
	
	/*如果有全页写，那么到FPI中查找数据*/
	if (XLogRecHasBlockImage(r, 0))
	{
		HeapTupleHeader			tuphdr = NULL;
		int						tuplelen = 0;
		ItemId					id = NULL;

		if(!get_image_from_store(&target_node, forknum, blknum, (char*)page, &pageindex))
		{
			/*不应该到达的代码*/
			elog(ERROR,"can not find imagestore about reloid=%u,blckno=%u", target_node.relNode, blknum);
		}

		walminer_debug("[walminer_decode_insert] xlrec->offnum=%d", xlrec->offnum);
		id = PageGetItemId(page, xlrec->offnum);
		tuphdr = (HeapTupleHeader)PageGetItem(page, id);
		tuplelen = (Size)id->lp_len;

		change->data.tp.newtuple =
			get_tuple_space(tuplelen);
		
		reassemble_tuple_from_heap_tuple_header(tuphdr, tuplelen, change->data.tp.newtuple);

	}
	else
	{
		walminer_debug("[walminer_decode_insert] tuple");
		if(XLOG_HEAP_INIT_PAGE & XLogRecGetInfo(r))
		{
			page_init_by_xlog(&target_node, forknum, blknum);
		}

		tupledata = XLogRecGetBlockData(r, 0, &datalen);
		tuplelen = datalen - SizeOfHeapHeader;

		change->data.tp.newtuple =
			get_tuple_space(tuplelen);

		reassemble_tuple_from_wal_data(tupledata, datalen, change, xid, InvalidTransactionId, true);

		
		/* 
		 * 此处为向image redo的代码
		 * 
		 * 如果找到了前面的全页写数据且无FPI,应该更新我们记录的image信息
		 * 如果没有找到，说明没有找到开始的checkpoint信息,此处不再多作处理
		 */
		if(get_image_from_store(&target_node, forknum, blknum, (char*)page, &pageindex))
		{
			HeapTupleHeader 		htup = NULL;
			Size					newlen = 0;

			walminer_debug("[walminer_decode_insert] redo to image");
			htup = change->data.tp.newtuple->tuple.t_data;
			newlen =  change->data.tp.newtuple->tuple.t_len;
			if(InvalidOffsetNumber == PageAddItem(page, (Item) htup, newlen, xlrec->offnum, true, true))
				elog(ERROR, "failed to add tuple:offsetnum %d,relnode %u, blckno %d", xlrec->offnum, target_node.relNode, blknum);
			flush_page(pageindex, page);
			//out_page_to_file(page, target_node.relNode, blknum, 2);
		}
		else
		{
			if(!regression_mode)
				elog(NOTICE, "Can not find FPW for relnode %u, blckno %d, ignore redo.",
														target_node.relNode, blknum);
		}
	}
	change->data.tp.clear_toast_afterwards = true;
	xid = XLogRecGetXid(wdc->reader);
	append_change_to_list(xid, change);
}

/*
 * 将wal记录decode为change格式
 * TODO(lchch)从image中读取数据，填充tuple
 */
static void
walminer_decode_delete(WalRecordBuffer *buf)
{
	XLogReaderState 		*r = buf->record;
	xl_heap_delete 			*xlrec = NULL;
	ReorderBufferChange 	*change = NULL;
	RelFileNode 			target_node;
	TransactionId			xid = 0;
	ForkNumber 				forknum = InvalidForkNumber;
	BlockNumber 			blknum = InvalidBlockNumber;
	int						pageindex = 0;
	char					page[BLCKSZ] = {0};
	WalmierDecodingContext *wdc = walminer_decode_context;
	bool					get_image = false;
	Size					where_len_inwal = XLogRecGetDataLen(r) - SizeOfHeapDelete; /* delete wal record中，数据的大小 */

	memset(&target_node, 0, sizeof(RelFileNode));

	xlrec = (xl_heap_delete *) XLogRecGetData(r);

	/* only interested in our database */
	wm_XLogRecGetBlockTag(r, 0, &target_node, &forknum, &blknum);
	if (!filter_in_decode(&target_node))
		return;

	change = get_change_space();
	change->action = REORDER_BUFFER_CHANGE_DELETE;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(GET_CHANGE_RELNODE(change), &target_node, sizeof(RelFileNode));
	change->lsn = r->ReadRecPtr;
	get_image = XLogRecHasBlockImage(r, 0);

	/*如果有全页写，那么到FPI中查找数据*/
	if (XLogRecHasBlockImage(r, 0) || !(xlrec->flags & XLH_DELETE_CONTAINS_OLD_TUPLE))
	{
		HeapTupleHeader			tuphdr = NULL;
		int						tuplelen = 0;
		ItemId					id = NULL;

		walminer_debug("[walminer_decode_delete] from image");
		
		if(!get_image_from_store(&target_node, forknum, blknum, (char*)page, &pageindex))
		{
			walminer_debug("[walminer_decode_delete] no image");
			if(get_image)
			{
				;/* 不可到达 */
			}
			else
			{
				/*
				* 没有找到FPW数据,这在开始解析时没有找到全页写数据的情况下会发生
				* 在事务完成解析时，会处理这种情况
				*/
				;
			}
		}
		else
		{
			walminer_debug("[walminer_decode_delete] get image");
			id = PageGetItemId(page, xlrec->offnum);
			tuphdr = (HeapTupleHeader)PageGetItem(page, id);
			tuplelen = (Size)id->lp_len;

			change->data.tp.oldtuple = get_tuple_space(tuplelen);
			
			reassemble_tuple_from_heap_tuple_header(tuphdr, tuplelen, change->data.tp.oldtuple);
		}
	}
	else
	{
		Size		tuplelen = where_len_inwal - SizeOfHeapHeader;

		walminer_debug("[walminer_decode_delete] get tuple where_len_inwal=%ld tuplelen=%ld", where_len_inwal, tuplelen);

		Assert(XLogRecGetDataLen(r) > (SizeOfHeapDelete + SizeOfHeapHeader));

		change->data.tp.oldtuple = get_tuple_space(tuplelen);

		reassemble_tuple_from_wal_data((char *) xlrec + SizeOfHeapDelete, where_len_inwal, change, 
								InvalidTransactionId, xlrec->xmax, false);
		
		/* 
		 * 此处为向image redo的代码
		 * 
		 * 如果找到了前面的全页写数据且无FPI,应该更新我们记录的image信息
		 * 如果没有找到，说明没有找到开始的checkpoint信息,此处不再多作处理
		 */
		if(get_image_from_store(&target_node, forknum, blknum, (char*)page, &pageindex))
		{

			walminer_debug("[walminer_decode_delete] get redo to image");
			PageSetPrunable(page,  XLogRecGetXid(r));
			flush_page(pageindex, page);
		}
		else
		{
			if(!regression_mode)
				elog(NOTICE, "Can not find FPW for relnode %u, blckno %d, ignore redo.",
														target_node.relNode, blknum);
		}
	}

	change->data.tp.clear_toast_afterwards = true;
	xid = XLogRecGetXid(wdc->reader);

	append_change_to_list(xid, change);
}

/*
 * 将wal记录decode为change格式
 * TODO(lchch)从image中读取数据，填充tuple
 */
static void
walminer_decode_update(WalRecordBuffer *buf)
{
	XLogReaderState 		*r = buf->record;
	xl_heap_update 			*xlrec = NULL;
	ReorderBufferChange 	*change = NULL;
	char	  				*data = NULL;
	RelFileNode 			target_node;
	ForkNumber 				forknum = InvalidForkNumber;
	BlockNumber 			blknum_new = InvalidBlockNumber;
	int						pageindex_new = 0;
	char					page_new[BLCKSZ] = {0};
	BlockNumber 			blknum_old = InvalidBlockNumber;
	int						pageindex_old = 0;
	char					page_old[BLCKSZ] = {0};
	bool					old_image_exsits = false;

	memset(&target_node, 0, sizeof(RelFileNode));

	xlrec = (xl_heap_update *) XLogRecGetData(r);

	/* only interested in our database */
	wm_XLogRecGetBlockTag(r, 0, &target_node, &forknum, &blknum_new);
	if (!XLogRecGetBlockTagExtended(r, 1, NULL, NULL, &blknum_old, NULL))
	{
		blknum_old = blknum_new;
		old_image_exsits = XLogRecHasBlockImage(r, 0);
	}
	else
		old_image_exsits = XLogRecHasBlockImage(r, 1);

	if (!filter_in_decode(&target_node))
		return;

	change = get_change_space();
	change->action = REORDER_BUFFER_CHANGE_UPDATE;
	change->origin_id = XLogRecGetOrigin(r);
	memcpy(GET_CHANGE_RELNODE(change), &target_node, sizeof(RelFileNode));

	change->lsn = r->ReadRecPtr;

	if (XLogRecHasBlockImage(r, 0))
	{
		HeapTupleHeader			tuphdr = NULL;
		int						tuplelen = 0;
		ItemId					id = NULL;

		walminer_debug("[walminer_decode_update] new image");
		
		if(!get_image_from_store(&target_node, forknum, blknum_new, (char*)page_new, &pageindex_new))
		{
			/*不应该到达的代码*/
			elog(ERROR,"can not find imagestore about reloid=%u,blckno=%u", target_node.relNode, blknum_new);
		}

		id = PageGetItemId(page_new, xlrec->new_offnum);
		tuphdr = (HeapTupleHeader)PageGetItem(page_new, id);
		tuplelen = (Size)id->lp_len;

		change->data.tp.newtuple =
			get_tuple_space(tuplelen);
		
		reassemble_tuple_from_heap_tuple_header(tuphdr, tuplelen, change->data.tp.newtuple);
	}
	else
	{
		Size		datalen;

		if(XLOG_HEAP_INIT_PAGE & XLogRecGetInfo(r))
		{
			page_init_by_xlog(&target_node, forknum, blknum_new);
		}

		walminer_debug("[walminer_decode_update] get new tuple");

		data = XLogRecGetBlockData(r, 0, &datalen);

		reassemble_tuplenew_from_wal_data(data, datalen, change, xlrec, blknum_new, blknum_old, XLogRecGetXid(r));

		/* 
		 * 此处为向image redo的代码
		 * 
		 * 如果找到了前面的全页写数据且无FPI,应该更新我们记录的image信息
		 * 如果没有找到，说明没有找到开始的checkpoint信息,此处不再多作处理
		 */
		if(get_image_from_store(&target_node, forknum, blknum_new, (char*)page_new, &pageindex_new))
		{
			HeapTupleHeader 		htup = NULL;
			Size					newlen = 0;

			walminer_debug("[walminer_decode_update] new redo to image");

			htup = change->data.tp.newtuple->tuple.t_data;
			newlen =  change->data.tp.newtuple->tuple.t_len;
			if(InvalidOffsetNumber == PageAddItem(page_new, (Item) htup, newlen, xlrec->new_offnum, true, true))
				elog(ERROR, "failed to add tuple:offsetnum %d,relnode %u, blckno %d", xlrec->new_offnum, target_node.relNode, blknum_new);
			flush_page(pageindex_new, page_new);
		}
		else
		{
			if(!regression_mode)
				elog(NOTICE, "Can not find FPW for relnode %u, blckno %d, ignore redo.",
														target_node.relNode, blknum_new);
		}
	}

	if (old_image_exsits || !(xlrec->flags & XLH_UPDATE_CONTAINS_OLD))
	{
		HeapTupleHeader			tuphdr = NULL;
		int						tuplelen = 0;
		ItemId					id = NULL;
		
		if(!get_image_from_store(&target_node, forknum, blknum_old, (char*)page_old, &pageindex_old))
		{
			if(old_image_exsits)
			{
				//不可能到达
				elog(ERROR, "[walminer_decode_update] Can not find image from store");
			}
			else
			{
				/*
				 * 无法找到条件元组
				 * 在没有找到上一个checkpoint点时会出现这个情况
				 */
				elog(NOTICE, "[walminer_decode_update] Can not find image from store");
			}

		}
		else
		{
			id = PageGetItemId(page_old, xlrec->old_offnum);
			tuphdr = (HeapTupleHeader)PageGetItem(page_old, id);
			tuplelen = (Size)id->lp_len;

			change->data.tp.oldtuple = get_tuple_space(tuplelen);
			
			walminer_debug("[walminer_decode_update] get old tuple from image");
			
			reassemble_tuple_from_heap_tuple_header(tuphdr, tuplelen, change->data.tp.oldtuple);
		}
	}
	else
	{
		Size		datalen;
		Size		tuplelen;

		/* caution, remaining data in record is not aligned */
		data = XLogRecGetData(r) + SizeOfHeapUpdate;
		datalen = XLogRecGetDataLen(r) - SizeOfHeapUpdate;
		tuplelen = datalen - SizeOfHeapHeader;

		change->data.tp.oldtuple = get_tuple_space(tuplelen);

		walminer_debug("[walminer_decode_update] get old tuple");

		reassemble_tuple_from_wal_data(data, datalen, change, InvalidTransactionId, xlrec->old_xmax, false);
		/* 
		 * 此处为向image redo的代码
		 * 
		 * 如果找到了前面的全页写数据且无FPI,应该更新我们记录的image信息
		 * 如果没有找到，说明没有找到开始的checkpoint信息,此处不再多作处理
		 */
		if(get_image_from_store(&target_node, forknum, blknum_old, (char*)page_old, &pageindex_old))
		{
			walminer_debug("[walminer_decode_update] old tuple redo to image");
			PageSetPrunable(page_old,  XLogRecGetXid(r));
			flush_page(pageindex_old, page_old);
		}
		else
		{
			if(!regression_mode)
				elog(NOTICE, "Can not find FPW for relnode %u, blckno %d, ignore redo.",
													target_node.relNode, blknum_old);
		}
	}

	change->data.tp.clear_toast_afterwards = true;
	append_change_to_list( XLogRecGetXid(r), change);
}

bool
filter_in_decode(RelFileNode *target_node)
{
	Oid	reloid = InvalidOid;
	int	relkind = 0;
	/* 不关心非当前数据库 */
	if (target_node->dbNode != walminer_decode_context->dict_dboid && target_node->spcNode != 0 &&
																target_node->spcNode != 1664)
		return false;
	
	reloid = get_reloid_by_relfilenode(target_node);

	if(wdecoder.w_call_funcs.walminer_filter_in_decode &&
			!wdecoder.w_call_funcs.walminer_filter_in_decode(target_node, reloid))
	{
		return false;
	}

	/* 当前不在数据字典中找不到的表 */
	if(WALMINER_LOST_RECORD == reloid)
	{
		List		*missd_relfilenode = NULL;
		ListCell	*cell = NULL;

		missd_relfilenode =  walminer_decode_context->anapro.missd_relfilenode;
		foreach(cell, missd_relfilenode)
		{
			Oid	temp_relfilenode = 0;

			temp_relfilenode = (Oid)lfirst_oid(cell);
			if(temp_relfilenode == target_node->relNode)
				return false;
		}
		walminer_decode_context->anapro.missd_relfilenode = lappend_oid(missd_relfilenode,
															target_node->relNode);
		if(regression_mode)
			elog(NOTICE, "Con not find relfilenode XXX in dictionary, ignored related records");
		else
			elog(NOTICE, "Con not find relfilenode %u in dictionary, ignored related records",
																		target_node->relNode );
		return false;
	}

	/* 只关心RELKIND_RELATION,RELKIND_PARTITIONED_TABLE,RELKIND_TOASTVALUE */
	relkind = get_relkind_by_reloid(reloid);
	if(!relkind_is_normalrel(relkind) && !relkind_is_toastrel(relkind))
	{
		return false;
	}

	return true;
}

void
big_transaction_indraft(TransactionEntry *te)
{
	char			transaction_file_path[MAXPGPATH] = {0};
	char			t_file_name[NAMEDATALEN] = {0};
	int				loop = 0;
	TxDiskHead		tdh;
	int				rlen = 0;
	FILE			*fp = NULL;

	Assert(te->disked);

	sprintf(t_file_name, "xid_%d.spill", te->xid);
	get_transaction_pach(transaction_file_path, t_file_name);
	if(!te->fp)
	{
		te->fp = fopen(transaction_file_path, "rb");
		if(!te->fp)
		{
			elog(ERROR, "[big_transaction_indraft]Can not open file %s to read:%m", transaction_file_path);
		}
	}
	fp = te->fp;
	walminer_debug("[big_transaction_indraft] xid=%u, te->nentries_mem=%d", 
														te->xid, te->nentries_mem);
	for(; loop < wdecoder.max_records_per_tx; loop++)
	{
		ReorderBufferChange		*change = NULL;

		memset(&tdh, 0, sizeof(TxDiskHead));
		change = get_change_space();

		rlen = fread(&tdh, 1, sizeof(TxDiskHead), fp);
		if(0 == rlen)
		{
			te->finish_indraft = true;
			fclose(te->fp);
			te->fp = NULL;
			fp = NULL;
			break;
		}
		if(sizeof(TxDiskHead) != rlen)
		{
			elog(ERROR, "Wrong length %d read from %s for TxDiskHead", rlen, transaction_file_path);
		}

		rlen = fread(change, 1, sizeof(ReorderBufferChange), fp);
		change->data.tp.newtuple = NULL;
		change->data.tp.oldtuple = NULL;
		if(sizeof(ReorderBufferChange) != rlen)
		{
			elog(ERROR, "Wrong length %d read from %s for ReorderBufferChange", rlen, transaction_file_path);
		}

		if(tdh.get_old)
		{
			change->data.tp.oldtuple = get_tuple_space(tdh.old_len);
			rlen = fread(change->data.tp.oldtuple, 1, sizeof(ReorderBufferTupleBuf), fp);
			change->data.tp.oldtuple->tuple.t_data =  ReorderBufferTupleBufData(change->data.tp.oldtuple);
			if(sizeof(ReorderBufferTupleBuf) != rlen)
			{
				elog(ERROR, "Wrong length %d read from %s for ReorderBufferTupleBuf", rlen, transaction_file_path);
			}
		}
		if(tdh.get_new)
		{
			change->data.tp.newtuple = get_tuple_space(tdh.new_len);
			rlen = fread(change->data.tp.newtuple, 1, sizeof(ReorderBufferTupleBuf), fp);
			change->data.tp.newtuple->tuple.t_data =  ReorderBufferTupleBufData(change->data.tp.newtuple);
			if(sizeof(ReorderBufferTupleBuf) != rlen)
			{
				elog(ERROR, "Wrong length %d read from %s for ReorderBufferTupleBuf", rlen, transaction_file_path);
			}
		}

		if(tdh.get_old)
		{
			
			rlen = fread(change->data.tp.oldtuple->tuple.t_data, 1, tdh.old_len, fp);
			if(tdh.old_len != rlen)
			{
				elog(ERROR, "Wrong length %d read from %s for old_len", rlen, transaction_file_path);
			}
		}
		if(tdh.get_new)
		{
			rlen = fread(change->data.tp.newtuple->tuple.t_data, 1, tdh.new_len, fp);
			if(tdh.new_len != rlen)
			{
				elog(ERROR, "Wrong length %d read from %s for new_len", rlen, transaction_file_path);
			}
		}
		
		te->changes = lappend(te->changes, change);
		te->nentries_mem++;
	}


	walminer_debug("indraft %d chnages in transaction %u from disk", te->nentries_mem, te->xid);
}

/*
 * 在一个变更数据很多的事务中,recorderbuffer可能会很多数据，因此这种情况下需要将
 * 数据暂存到磁盘中。
 */
void
big_transaction_spill(TransactionEntry *te, bool focus)
{
	char			transaction_file_path[MAXPGPATH] = {0};
	char			t_file_name[NAMEDATALEN] = {0};
	FILE			*fp = NULL;
	ListCell		*cell = NULL;
	TxDiskHead		tdh;

	walminer_debug("[big_transaction_spill] xid=%u, te->nentries_mem=%d， focus=%d", 
														te->xid, te->nentries_mem, focus);
	if(te->nentries_mem < wdecoder.max_records_per_tx && !focus)
	{
		return;
	}

	sprintf(t_file_name, "xid_%d.spill", te->xid);
	get_transaction_pach(transaction_file_path, t_file_name);

	fp = fopen(transaction_file_path, "ab");
	if(!fp)
	{
		elog(ERROR, "[big_transaction_spill]Can not open file %s to write:%m", transaction_file_path);
	}
	walminer_debug("spill %d chnages in transaction %u to disk", te->nentries_mem, te->xid);
	
	foreach(cell, te->changes)
	{
		ReorderBufferTupleBuf 	*oldtup = NULL, *newtup = NULL;
		ReorderBufferChange		*change = NULL;
		int						wlen = 0;

		change = lfirst(cell);
		Assert(REORDER_BUFFER_CHANGE_UPDATE == change->action ||
			REORDER_BUFFER_CHANGE_DELETE == change->action ||
			REORDER_BUFFER_CHANGE_INSERT == change->action);

		oldtup = change->data.tp.oldtuple;
		newtup = change->data.tp.newtuple;

		memset(&tdh, 0, sizeof(TxDiskHead));
		tdh.total_size += sizeof(TxDiskHead);
		tdh.total_size += sizeof(ReorderBufferChange);
		if(oldtup)
		{
			tdh.get_old = true;
			tdh.total_size += sizeof(ReorderBufferTupleBuf);
			tdh.total_size += oldtup->tuple.t_len;
			tdh.old_len = oldtup->tuple.t_len;
		}
		if(newtup)
		{
			tdh.get_new = true;
			tdh.total_size += sizeof(ReorderBufferTupleBuf);
			tdh.total_size += newtup->tuple.t_len;
			tdh.new_len = newtup->tuple.t_len;
		}

		wlen = fwrite(&tdh, 1, sizeof(TxDiskHead), fp);
		if(sizeof(TxDiskHead) != wlen)
		{
			elog(ERROR, "Fail to write %d size to %s", wlen, transaction_file_path);
		}

		wlen = fwrite(change, 1, sizeof(ReorderBufferChange), fp);
		if(sizeof(ReorderBufferChange) != wlen)
		{
			elog(ERROR, "Fail to write %d size to %s", wlen, transaction_file_path);
		}

		if(tdh.get_old)
		{
			wlen = fwrite(oldtup, 1, sizeof(ReorderBufferTupleBuf), fp);
			if(sizeof(ReorderBufferTupleBuf) != wlen)
			{
				elog(ERROR, "Fail to write %d size to %s", wlen, transaction_file_path);
			}
		}
		if(tdh.get_new)
		{
			wlen = fwrite(newtup, 1, sizeof(ReorderBufferTupleBuf), fp);
			if(sizeof(ReorderBufferTupleBuf) != wlen)
			{
				elog(ERROR, "Fail to write %d size to %s", wlen, transaction_file_path);
			}
		}

		if(tdh.get_old)
		{
			wlen = fwrite(oldtup->tuple.t_data, 1, oldtup->tuple.t_len, fp);
			if(oldtup->tuple.t_len != wlen)
			{
				elog(ERROR, "Fail to write %d size to %s", wlen, transaction_file_path);
			}
		}
		if(tdh.get_new)
		{
			wlen = fwrite(newtup->tuple.t_data, 1, newtup->tuple.t_len, fp);
			if(newtup->tuple.t_len != wlen)
			{
				elog(ERROR, "Fail to write %d size to %s", wlen, transaction_file_path);
			}
		}
	}

	fclose(fp);
	fp = NULL;
	te->disked = true;
	te->nentries_mem = 0;
	walminer_debug("[big_transaction_spill] xid=%u, te->nentries_mem=%d", te->xid, te->nentries_mem);
	free_change_list(te->changes);
	te->changes = NULL;
}

void*
decode_wal(void)
{
    XLogRecPtr              first_record = InvalidXLogRecPtr;

	if(wdecoder.w_call_funcs.walminer_wait_search)
	{
		if(!wdecoder.w_call_funcs.walminer_wait_search())
			return NULL;
	}

	first_record = prepare_read(false, InvalidXLogRecPtr);

	clean_image();
#if (defined PG_VERSION_13) || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
	XLogBeginRead(walminer_decode_context->reader, first_record);
#endif
	
    while(true)
    {
        get_next_record(walminer_decode_context->reader, walminer_decode_context->decode_prive ,first_record);
        first_record = InvalidXLogRecPtr;

		if(wdecoder.w_call_funcs.walminer_check_statue)
			if(!wdecoder.w_call_funcs.walminer_check_statue())
				break;
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
		if(!walminer_decode_context->reader->record)
#else
		if(!walminer_decode_context->reader->decoded_record)
#endif
			break;

		walminer_debug("[decode_wal] start_lsn=%x/%x",(uint32)(walminer_decode_context->reader->ReadRecPtr >> 32), 
												  (uint32)walminer_decode_context->reader->ReadRecPtr);
		walminer_decode_record(walminer_decode_context->reader);

    }
	walminer_debug("decode_wal end");
	return NULL;
}
