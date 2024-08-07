/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  pagecollect.c
 * 
 * 对数据页的操作代码在imagemanage.c中，这个代码文件意图实现一套机制，允许用
 * 户从wal日志中收集目标数据页。
 * 
 * TODO:以一个basebackup开始收集数据页
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "wm_utils.h"
#include "utils/builtins.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "storage/smgr.h"

#if ((defined PG_VERSION_13) || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16))
#include "access/relation.h"
#else
#include "catalog/catalog.h"
#include "access/heapam.h"
#endif

#define		WALMINER_PC_MAX_PAGE_NUM	10
#define 	MAG_BLOCK_FILENO(blockno) (blockno/RELSEG_SIZE)

PG_FUNCTION_INFO_V1(page_collect_internal);

typedef	struct PcStruct
{
    /* 入参 */
    Oid                 relnode_in_wal;
	Oid					oid_in_analyse_db;
	char				*page_array_str;
	bool				diff_table_report;

	/* 解析所用的数据 */
	RelFileNode			node_in_dict;
	RelFileNode			node_in_db;									/* 优先使用数据字典的数据填充，如果未找到可以使用当前数据库的数据进行填充 */
	BlockNumber			page_array[WALMINER_PC_MAX_PAGE_NUM];		/* 从page_array_str中解析出的每一个page号 */
	int					page_num;	/* 如果为-1则为解析所有的page */
}PcStruct;

static void set_call_back_funcs(void);
static bool pc_filter_in_decode(RelFileNode *target_node, Oid reloid);
static void pc_set_pages(void);
static void pc_handle_argument(PG_FUNCTION_ARGS);
static void* pc_search_wal(void* temp);
static bool pc_wait_search(void);
static void checkRelFileNode(Oid tbsoid,Oid relnode, BlockNumber targetBlock);

PcStruct    pc_struct;

static void
init_pc_struct(void)
{
    memset(&pc_struct, 0, sizeof(PcStruct));
}

static void
set_call_back_funcs(void)
{
	wdecoder.w_call_funcs.walminer_filter_in_decode = pc_filter_in_decode;
	wdecoder.w_call_funcs.walminer_set_pages = pc_set_pages;
	wdecoder.w_call_funcs.walminer_handle_argument = pc_handle_argument;
	wdecoder.w_call_funcs.walminer_search_wal = pc_search_wal;
	wdecoder.w_call_funcs.walminer_wait_search = pc_wait_search;
	wdecoder.w_call_funcs.walminer_wait_thread = wait_thread;
}

/*
 * decode阶段的过滤
 */
static bool
pc_filter_in_decode(RelFileNode *target_node, Oid reloid)
{
	/* 不需要解析系统表*/
	if(FirstNormalObjectId > reloid)
	{
		return false;
	}

	/* 单表解析处理代码 */
	if(target_node->relNode != pc_struct.relnode_in_wal)
    {
        return false;
    }
	return true;
}

static void
checkRelFileNode(Oid tbsoid, Oid relnode, BlockNumber targetBlock)
{
	char		path[MAXPGPATH] = {0};
	char		filePath[MAXPGPATH] = {0};
	uint32		relFileNum = 0;
	FILE		*fp = NULL;

	if(DEFAULTTABLESPACE_OID == tbsoid)
		sprintf(path, "%s/base/%u",DataDir,MyDatabaseId);
	else if(GLOBALTABLESPACE_OID == tbsoid)
		sprintf(path, "%s/global",DataDir);
	else
	{
		sprintf(path, "%s/pg_tblspc/%u/%s/%u",DataDir,tbsoid,TABLESPACE_VERSION_DIRECTORY,MyDatabaseId);
	}
	relFileNum = MAG_BLOCK_FILENO(targetBlock);
	if(0 != relFileNum)
		sprintf(filePath, "%s/%u.%u",path,relnode, relFileNum);
	else
		sprintf(filePath, "%s/%u",path,relnode);
	if(!is_file_exist(filePath))
	{
		fp = fopen(filePath,"wb+");
		if(!fp)
		{
			elog(ERROR,"Fail to create file \"%s\"",filePath);
		}
		fclose(fp);
	}
}

static void
pc_heap_sync(Relation rel)
{
	/* main heap */
	FlushRelationBuffers(rel);
	/* FlushRelationBuffers will have opened rd_smgr */
	smgrimmedsync(rel->rd_smgr, MAIN_FORKNUM);

	/* FSM is not critical, don't bother syncing it */

	/* toast heap, if any */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
	{
		Relation	toastrel;

		toastrel = relation_open(rel->rd_rel->reltoastrelid, AccessShareLock);
		FlushRelationBuffers(toastrel);
		if(toastrel->rd_smgr)
			smgrimmedsync(toastrel->rd_smgr, MAIN_FORKNUM);
		relation_close(toastrel, AccessShareLock);
	}
}

static void
replace_filenode_page(BlockNumber blockno, char *page)
{
	Oid			reloid = InvalidOid;
	Buffer		buffer = InvalidBuffer;
	char 		*page_in_buffer = NULL;
	Relation	rel = NULL;

	reloid = pc_struct.oid_in_analyse_db;
	rel = relation_open(reloid, AccessExclusiveLock);
	if(!rel)
	{
		elog(ERROR, "fail to open relation with oid \"%u\"",reloid);
	}
	buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blockno, RBM_ZERO_AND_LOCK, NULL);
	if(!buffer)
	{
		elog(ERROR,"fail to read buffer of blokno\"%u\"",blockno);
	}
	page_in_buffer = BufferGetPage(buffer);

	memcpy(page_in_buffer, page, BLCKSZ);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
	pc_heap_sync(rel);
	relation_close(rel, AccessExclusiveLock);
}

static bool
page_in_array(BlockNumber pageno)
{
	bool 	result = false;
	int		loop = 0;

	if(0 == pc_struct.page_num)
		return true;
	for(loop = 0; loop < pc_struct.page_num; loop++)
	{
		if(pageno == pc_struct.page_array[loop])
		{
			result = true;
			break;
		}
	}
	return result;
}

static void
pc_set_pages(void)
{
	char		page[BLCKSZ] = {0};
	int			pageindex = 0;
	bool		getpage = false;
	int			loop = 0;
	BlockNumber	maxno = InvalidBlockNumber;
	BlockNumber minno = InvalidBlockNumber;
	bool		inited = false;

	get_pageno_range(&maxno, &minno, &inited);
	walminer_debug("[pc_set_pages]maxno=%u,minno=%u,inited=%d", maxno, minno, inited);
	for(loop = minno; loop <= maxno; loop++)
	{
		if(!page_in_array(loop))
			continue;
		memset(page, 0, BLCKSZ);
		getpage = get_image_from_store(&pc_struct.node_in_dict, MAIN_FORKNUM, loop, page, &pageindex);
		if(getpage)
		{
			walminer_debug("[pc_set_pages]out_page_to_file pageno=%u",loop);
			checkRelFileNode(pc_struct.node_in_db.spcNode, pc_struct.node_in_db.relNode, loop);
			replace_filenode_page(loop, page);
		}
	}
}

static bool
pc_wait_search(void)
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
		/* 等待找到decode_lsn */
		pg_usleep(1 * 1000000L);
	}
	return true;
}

static void*
pc_search_wal(void* temp)
{
	WalmierDecodingContext		*wdc;

	wdc = walminer_decode_context;
	wdc->swpro.read_lsn = prepare_read(true, InvalidXLogRecPtr);
	wdc->swpro.decode_lsn = wdc->swpro.read_lsn;
	wdc->swpro.get_decode_lsn = true;
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
	return NULL;
}

static void
pc_handle_argument(PG_FUNCTION_ARGS)
{
	int		i = 0;
	char	*token = NULL;
    pc_struct.relnode_in_wal = PG_GETARG_OID(0);
	pc_struct.oid_in_analyse_db = PG_GETARG_OID(1);
	pc_struct.page_array_str = PG_GETARG_CSTRING(2);
	pc_struct.diff_table_report = PG_GETARG_BOOL(3);

	walminer_debug("ARGUMENT:relnode=%u,oid=%u, arr_str=%s",
			pc_struct.relnode_in_wal, pc_struct.oid_in_analyse_db, pc_struct.page_array_str);
	
	if(0 == strcmp("all", pc_struct.page_array_str))
	{
		pc_struct.page_num = 0;
	}
	else
	{
		/* 处理目标page字符串 */
		token = strtok(pc_struct.page_array_str, ",");
		while(token)
		{
			i++;
			if(WALMINER_PC_MAX_PAGE_NUM <= i)
			{
				elog(ERROR, "Can not support page numbers greate than %d", WALMINER_PC_MAX_PAGE_NUM);
			}
			trim_space(token);
			pc_struct.page_array[i-1] = atoi(token);
			token = strtok(NULL, ",");
		}
		pc_struct.page_num = i;
	}

	if(!get_relnode_by_relfilenode(pc_struct.relnode_in_wal, &pc_struct.node_in_dict))
	{
		if(pc_struct.diff_table_report)
			elog(ERROR, "Can not find relnode(%u) in data dictionary", pc_struct.relnode_in_wal);
		pc_struct.node_in_dict.dbNode = wdd.ddh.dboid;
		pc_struct.node_in_dict.spcNode = 0;
		pc_struct.node_in_dict.relNode = pc_struct.relnode_in_wal;
	}
	get_relnode_by_reloid(pc_struct.oid_in_analyse_db, &pc_struct.node_in_db);

	if(debug_mode)
	{
		wdecoder.debugfp = prepare_debugfile();
	}
}

/* 
 * 
 * 参数1：为在wal日志中需要解析的relfilenode(必填项)
 * 参数2：解析结果page覆盖的表的OID(必填项)
 * 参数3：需要收集的page号，格式为'0,1,2',最多为100个，默认为all即为解析所有的page
 * 参数4：relfilenode的表结构无法确定时是否报错,默认true
 * 
 */
Datum
page_collect_internal(PG_FUNCTION_ARGS)
{
	const char* result = "page collect success";
	check_all();

	memset(&wdecoder, 0, sizeof(WalminerDecode));
    init_pc_struct();
	set_call_back_funcs();

	pg_walminer(fcinfo);
    
	PG_RETURN_TEXT_P(cstring_to_text(result));
}
