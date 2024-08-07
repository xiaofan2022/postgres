/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  walminer.c
 * 
 * 执行walminer解析会在data数据目录下生成pg_walminer目录
 * 每个walminer接口都会检查pg_walminer目录完整性，缺失则创建。
 * 		data
 * 		  |--pg_walminer
 *  		  	|--wm_analyselog
 * 		  		|--wm_wallist
 * 		 		|--wm_image
 * 
 * 解析过程分为： 0.清空解析环境(非必须)   1.导出数据字典(非必须) 
 * 				 2.加载数据字典(非必须)   3.加载wal文件(非必须)
 * 				 4. 执行解析;
 * 
 * 0.执行一次解析之前需要清空解析目录，这个操作将wm_analyselog，wm_datadict，wm_image里的数据清除
 * 
 * 1.可以从生产库导出数据字典，拿到解析库执行解析。也可在生产库导出数据字典自用。
 * 
 * 2.加载数据字典到内存
 * 
 * 3.可多次添加不同目录下的wal文件。不可加载与数据字典时间线不一致的wal文件，参数
 * 可为wal文件也可为目录。如检测到未加载数据字典，则临时从本数据库产生数据字典到
 * 数据字典目录
 * 
 * 4. 只解析连续的wal日志,从加载的最小的wal日志开始计算,如果遇到wal文件缺失，则停止解析
 * 
 * 
 * 
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "datadictionary.h"
#include "fmgr.h"
#include "funcapi.h"
#include "wm_utils.h"
#include "walminer_decode.h"
#include "utils/pg_lsn.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "wal2sql_ddl.h"
#include "wal2sql.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(walminer_mrecords_inmemory);
PG_FUNCTION_INFO_V1(walminer_debug_mode);
PG_FUNCTION_INFO_V1(walminer_regression_mode);
PG_FUNCTION_INFO_V1(walminer_build_dictionary);
PG_FUNCTION_INFO_V1(walminer_load_dictionary);
PG_FUNCTION_INFO_V1(walminer_wal_add);
PG_FUNCTION_INFO_V1(walminer_wal_list);
PG_FUNCTION_INFO_V1(walminer_wal_remove);
PG_FUNCTION_INFO_V1(pg_walusage);
PG_FUNCTION_INFO_V1(walminer_table_avatar);
PG_FUNCTION_INFO_V1(walminer_stop);
PG_FUNCTION_INFO_V1(walminer_version);
PG_FUNCTION_INFO_V1(walminer_help);

void walminer_context_manage(void);

static char	*help_array[] = {
	"walminer_build_dictionary()",
	"walminer_load_dictionary()",
	"walminer_wal_add()",
	"walminer_wal_list()",
	"walminer_wal_remove()",
	"walminer_all()",
	"wal2sql()",
	"walminer_by_time()",
	"walmner_by_lsn()",
	"walminer_by_xid()",
	"walminer_apply()",
	"walminer_debug_mode()",
	"wal2sql_with_catalog()",
	"wal2sql_with_ddl()",
	"walminer_table_avatar()",
	"walminer_version()",
	"walminer_help()"
};

int					auto_record_in_memory = MAX_CHANGES_IN_MEMORY;
bool				regression_mode = false;
MemoryContext		walminer_context = NULL;
WalminerDecode		wdecoder;

void
walminer_context_manage(void)
{
	if(walminer_context)
	{
		return;
	}
	walminer_context = AllocSetContextCreate(TopMemoryContext,
							"Walminer context",
							ALLOCSET_DEFAULT_SIZES);
	MemoryContextAllowInCriticalSection(walminer_context, true);
}

void
walminer_stop_internal(void)
{
	char	temppath[MAXPGPATH] = {0};
	char	*dict_path = NULL;
	char	*runtemp_path = NULL;
	
	walminer_context_manage();

	//为了方便回归测试stop时不清理回归测试flag
	/* regression_mode = false; */
	debug_mode = false;

	end_logfile();
	end_debug_file();
	/*
	 * 删除不load数据字典时，产生的数据字典文件
	 */
	dict_path = get_dict_path(temppath);
	remove(dict_path);

	/*
	 * 删除运行时产生的文件
	 */
	runtemp_path = get_runtemp_path(temppath);
	drop_allfile_in_dir(runtemp_path);

	/*free在TopMemoryContext中为数据字典和wallist创建的空间*/
	free_wallist();
	free_dictionary();
	
	MemoryContextDelete(walminer_context);
	walminer_context = NULL;
	walminer_decode_context = NULL;
}

void
self_load_dic_and_wal(void)
{
	MemoryContext 		oldcxt = NULL;
	if(!regression_mode)
		elog(NOTICE, "Add wal from current pg_wal directory, do not suggest use this way in produce");
	walminer_context_manage();
	oldcxt = MemoryContextSwitchTo(walminer_context);
	add_wal(make_absolute_path(XLOGDIR));
	MemoryContextSwitchTo(oldcxt);
}

void
pg_walminer(PG_FUNCTION_ARGS)
{
	MemoryContext oldcontex = NULL;

	if(!regression_mode)
	{
		elog(NOTICE, "===========================================");
		elog(NOTICE, "======OPEN SOURCE PROJECT WALMINER=========");
		//elog(NOTICE, "======Author mail lchch1990@sina.cn========");
		elog(NOTICE, "===========================================");
	}
	pg_usleep(1000000L);
	if(!wal_file_list || !wdd.loaded)
	{
		self_load_dic_and_wal();
	}

	if(wdecoder.w_call_funcs.walminer_handle_argument)
		wdecoder.w_call_funcs.walminer_handle_argument(fcinfo);

	/* 将不连续的wal日志标记为无效 */
	go_through_list(wal_file_list);

	create_walminer_decode_context();
	
	oldcontex = MemoryContextSwitchTo(walminer_decode_context->context);
	/* walminer_search_wal()使用了非线程安全的一些PG内部函数，因此不能被多线程调用 */
	if(wdecoder.w_call_funcs.walminer_search_wal)
		wdecoder.w_call_funcs.walminer_search_wal(NULL);
	decode_wal();
	if(wdecoder.w_call_funcs.walminer_set_pages)
		wdecoder.w_call_funcs.walminer_set_pages();
	MemoryContextSwitchTo(oldcontex);
    
	walminer_stop_internal();
}


Datum
walminer_help(PG_FUNCTION_ARGS)
{
	StringInfo	strinfo = NULL;
	int			func_length = 0;
	int			loop = 0;

	strinfo = makeStringInfo();
	func_length = sizeof(help_array) / sizeof(char*);

	for(; loop < func_length; loop++)
	{
		appendStringInfo(strinfo, "%s\n",help_array[loop]);
	}

	PG_RETURN_CSTRING(strinfo->data);
}

Datum
walminer_mrecords_inmemory(PG_FUNCTION_ARGS)
{
	int 	record_in_memory = PG_GETARG_INT32(0);

	if(5 < record_in_memory)
	{
		auto_record_in_memory = record_in_memory;
	}
	PG_RETURN_INT32(auto_record_in_memory);
}

Datum
walminer_regression_mode(PG_FUNCTION_ARGS)
{

	regression_mode = true;
	PG_RETURN_BOOL(regression_mode);
}

Datum
walminer_debug_mode(PG_FUNCTION_ARGS)
{
	debug_mode = true;
	PG_RETURN_BOOL(debug_mode);
}

Datum
walminer_build_dictionary(PG_FUNCTION_ARGS)
{
	char 	*dict_path = PG_GETARG_CSTRING(0);
	
	check_all();
	build_dictionary(dict_path);
	PG_RETURN_CSTRING("Dictionary build success!");
}


Datum
walminer_load_dictionary(PG_FUNCTION_ARGS)
{
	char 	*dict_path = PG_GETARG_CSTRING(0);
	MemoryContext 		oldcxt = NULL;

	walminer_context_manage();

	oldcxt = MemoryContextSwitchTo(walminer_context);
	check_all();
	load_dictionary(dict_path, WALMINER_LOADDICT_MOVE);
	MemoryContextSwitchTo(oldcxt);
    PG_RETURN_CSTRING("Dictionary load success!");
}

Datum
walminer_wal_add(PG_FUNCTION_ARGS)
{
	char 	*wal_path = PG_GETARG_CSTRING(0);
	char	backstr[100] = {0};
	int		addnum = 0;
	MemoryContext 		oldcxt = NULL;

	walminer_context_manage();
	oldcxt = MemoryContextSwitchTo(walminer_context);

	check_all();
	addnum = add_wal(make_absolute_path(wal_path));

	sprintf(backstr, "%d file add success", addnum);
	MemoryContextSwitchTo(oldcxt);
	PG_RETURN_CSTRING(pstrdup((char*)backstr));
}

Datum
walminer_wal_remove(PG_FUNCTION_ARGS)
{
	char 				*wal_path = PG_GETARG_CSTRING(0);
	int					removenum = 0;
	char				backstr[100] = {0};

	removenum = remove_wal_file(wal_path);

	sprintf(backstr, "%d file remove success", removenum);
	PG_RETURN_CSTRING(pstrdup((char*)backstr));
}


Datum
walminer_wal_list(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx = NULL;
	ListCell		*cell = NULL;

	check_all();

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc		tupdesc = NULL;
		MemoryContext 	oldcxt = NULL;

		if(!wal_file_list)
			ereport(ERROR,(errmsg("wal list has not been loaded or has been removed.")));
		
		walminer_context_manage();
		oldcxt = MemoryContextSwitchTo(walminer_context);
		funcctx = SRF_FIRSTCALL_INIT();
		funcctx->user_fctx = list_head(wal_file_list);
		
		tupdesc = makeOutputXlogDesc();
		funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);
		MemoryContextSwitchTo(oldcxt);
	}

	funcctx = SRF_PERCALL_SETUP();
	cell = funcctx->user_fctx;
	while(cell)
	{
		HeapTuple	tuple = NULL;
		WalFile		*wf = NULL;
		char		*values[1];

		
		wf = lfirst(cell);
		values[0] = wf->filepath;

		tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
#if (defined PG_VERSION_13) || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
		funcctx->user_fctx = lnext(wal_file_list, cell);
#else
		funcctx->user_fctx = lnext(cell);
#endif
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));

	}
    SRF_RETURN_DONE(funcctx);
}

Datum
pg_walusage(PG_FUNCTION_ARGS)
{
	PG_RETURN_CSTRING(pstrdup("In Coding..."));
}

Datum
walminer_table_avatar(PG_FUNCTION_ARGS)
{
	char	*tablename_in_datadic = PG_GETARG_CSTRING(0);
	Oid		relfilenode_in_wal = PG_GETARG_OID(1);
	Oid		relfilenode_in_datadic = InvalidOid;
	char	result[100] = {0};

	MemoryContext 		oldcxt = NULL;

	walminer_context_manage();
	oldcxt = MemoryContextSwitchTo(walminer_context);

	relfilenode_in_datadic = set_user_relfilenode_map(tablename_in_datadic,
													relfilenode_in_wal);
	sprintf(result, "MAP[%s:%u]->%u", tablename_in_datadic, relfilenode_in_datadic,
													relfilenode_in_wal);
	MemoryContextSwitchTo(oldcxt);
	PG_RETURN_CSTRING(pstrdup(result));
}

Datum
walminer_stop(PG_FUNCTION_ARGS)
{
	walminer_stop_internal();

	PG_RETURN_CSTRING("walminer stoped!");
}

Datum
walminer_version(PG_FUNCTION_ARGS)
{
	char walminer_version[100] = {0};

	sprintf(walminer_version, "Walminer %s(dev) for PostgreSQL %s", WALMINER_VERSION_NUM, PACKAGE_VERSION);
	PG_RETURN_CSTRING(pstrdup(walminer_version));
}