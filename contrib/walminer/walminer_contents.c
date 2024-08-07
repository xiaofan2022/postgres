/*-------------------------------------------------------------------------
 *
 * walminer_contents.c
 *
 *-------------------------------------------------------------------------
*/

//#include "catalog/indexing.h"
#include "walminer_decode.h"
#include "wm_utils.h"
#include "walminer_contents.h"
#include "datadictionary.h"

#include "utils/builtins.h"
#include "datatype/timestamp.h"
#include "access/heapam.h"
#include "utils/rel.h"
#include "utils/pg_lsn.h"



#define WALMINER_OUTPUT_TABLENAME			"walminer_contents"

void
insert_walcontents_tuple(void *te_ptr)
{
	TransactionEntry	*te = NULL;
	Relation	walminer_contents = NULL;
	HeapTuple	tup = NULL;
	Oid			reloid = 0;
	char		*op_ptr = NULL;
	char		*undo_ptr = NULL;
	text		*op_text = NULL;
	text		*undo_text = NULL;
	text		*tablename = NULL;
	text		*schemaname = NULL;
	XLogRecPtr	start_lsn = InvalidXLogRecPtr;
	XLogRecPtr	commit_lsn = InvalidXLogRecPtr;
	Datum		values[Natts_walminer_contents];
	bool		nulls[Natts_walminer_contents];



	te = (TransactionEntry*) te_ptr;	
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	if(!te->sql.init)
	{
		/* 不应该发生 */
		elog(ERROR, "Why te->sql did not inited?");
	}

	if(te->sql.isddl)
	{
		op_ptr = te->sql.notsql.data;
		undo_ptr = NULL;
	}
	else if(te->sql.issql)
	{
		op_ptr = te->sql.sql_pro.data;
		undo_ptr = te->sql.sql_undo.data;
		start_lsn = te->start_lsn;
		commit_lsn = te->final_lsn;
		tablename = cstring_to_text(te->relname.data);
		schemaname = cstring_to_text(te->namespace.data);
	}
	else
	{
		op_ptr = te->sql.notsql.data;
		undo_ptr = NULL;
	}

	reloid =  walminer_decode_context->walminer_contents_oid;
	if(0 == reloid)
	{
		get_reloid_by_relname(WALMINER_OUTPUT_TABLENAME, &reloid, NULL, true);
		walminer_decode_context->walminer_contents_oid = reloid;
		walminer_debug("[insert_walcontents_tuple]out reloid=%u", reloid);
		if(!reloid)
			elog(ERROR,"It is failed to open temporary table walminer_contents.");/*should not happen*/
	}
	start_lsn = te->start_lsn;
	commit_lsn = te->final_lsn;
	tablename = cstring_to_text(te->relname.data);
	schemaname = cstring_to_text(te->namespace.data);

	values[Anum_walminer_contents_no - 1] = Int32GetDatum(++te->sqlno);
	values[Anum_walminer_contents_xid - 1] = Int64GetDatum(te->xid);
	values[Anum_walminer_contents_topxid - 1] = Int64GetDatum(te->topxid);
	values[Anum_walminer_contents_sqlkind - 1] = Int64GetDatum(te->sqlkind);
	values[Anum_walminer_contents_minerd - 1] = BoolGetDatum(te->cananalyse);
	values[Anum_walminer_contents_timestamp - 1] = TimestampTzGetDatum(te->commit_time);
	
	if(op_ptr)
	{
		op_text = cstring_to_text(op_ptr);
		values[Anum_walminer_contents_op_text - 1] = PointerGetDatum(op_text);
	}
	else
		nulls[Anum_walminer_contents_op_text - 1] = true;
	if(undo_ptr)
	{
		undo_text = cstring_to_text(undo_ptr);
		values[Anum_walminer_contents_undo_text - 1] = PointerGetDatum(undo_text);
	}
	else
		nulls[Anum_walminer_contents_undo_text - 1] = true;
	values[Anum_walminer_contents_complete - 1] = BoolGetDatum(te->complete);
	values[Anum_walminer_contents_schema - 1] = PointerGetDatum(schemaname);
	values[Anum_walminer_contents_relation - 1] = PointerGetDatum(tablename);
	values[Anum_walminer_contents_startlsn - 1] = LSNGetDatum(start_lsn);
	values[Anum_walminer_contents_commitlsn - 1] = LSNGetDatum(commit_lsn);



#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	walminer_contents = heap_open(reloid, AccessShareLock);
#else
	walminer_contents = table_open(reloid, AccessShareLock);
#endif
	if(!walminer_contents)
		ereport(ERROR,(errmsg("It is failed to open temporary table walminer_contents.")));

	tup = heap_form_tuple(RelationGetDescr(walminer_contents), values, nulls);
	simple_heap_insert(walminer_contents, tup);
	if(op_text)
		walminer_free((char*)op_text, 0);
	if(undo_text)
		walminer_free((char*)undo_text, 0);
	if(schemaname)
		walminer_free((char*)schemaname, 0);
	if(tablename)
		walminer_free((char*)tablename, 0);
	
	if(tup)
		heap_freetuple(tup);
	if(walminer_contents)
	{
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
		heap_close(walminer_contents, AccessShareLock);	
#else
		table_close(walminer_contents, AccessShareLock);
#endif
	}
}
