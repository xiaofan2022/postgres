/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  fetchcatalogtable.c
 *
 *-------------------------------------------------------------------------
 */

#include "datadictionary.h"
#include "utils/syscache.h"
#include "access/genam.h"
#include "utils/snapmgr.h"
#include "access/htup_details.h"
#include "access/heapam.h"
#include "wm_utils.h"
#include "datadictionary.h"
#include "utils/array.h"
#include "utils/rel.h"



static void add_tuple_to_cts(SingleCatalogCache *scc, HeapTuple tuple, int datasize);

bool
get_relation_oid_by_relname(char* relname, Oid* reloid, bool gettemptable)
{
	bool				result = false;
	Relation			pgclass = NULL;
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	HeapScanDesc		scan = NULL;
#else
	SysScanDesc		scan = NULL;
#endif
	HeapTuple			tuple = NULL;
	Form_pg_class 		classForm = NULL;
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	pgclass = heap_open(RelationRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(pgclass, 0, NULL);
#else
	pgclass = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(pgclass, 0, false,
							   SnapshotSelf, 0, NULL);
#endif

#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
#else
	while ((tuple = systable_getnext(scan)) != NULL)
#endif
	{
		classForm = (Form_pg_class) GETSTRUCT(tuple);
		if(0 == strcmp(relname,classForm->relname.data))
		{
			if(gettemptable && RELPERSISTENCE_TEMP != classForm->relpersistence)
				continue;
			if(!gettemptable && PG_CATALOG_NAMESPACE != classForm->relnamespace)
				continue;
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
			*reloid = HeapTupleHeaderGetOid(tuple->t_data);
#else
			*reloid = classForm->oid;
#endif
			result = true;
			break;
		}
	}
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	heap_endscan(scan);
	heap_close(pgclass, AccessShareLock);
#else
	systable_endscan(scan);
	table_close(pgclass, AccessShareLock);
#endif
	return result;
}

static void
add_tuple_to_cts(SingleCatalogCache *scc, HeapTuple tuple, int datasize)
{
	char	*data = NULL;
	
	data = GETSTRUCT(tuple) - sizeof(Oid);

	
	while(scc->totsize - scc->usesize < datasize)
		add_space_for_cts(scc, 0);
	
	memcpy(scc->cursor, data, datasize);
	scc->cth.elemnum++;
	scc->usesize += datasize;
	scc->cursor += datasize;
}

static void
add_space_for_cid(ConstrintInDic *cid)
{
	int			addstep = 1000;
	int			totalnum = 0;
	PKeyItem	*old_space = NULL;

	Assert(cid);
	walminer_debug("[add_space_for_cid]cid->malnum=%d,", cid->malnum);

	totalnum = addstep + cid->malnum;
	old_space = cid->pkilist;
	
	cid->pkilist = (PKeyItem*)walminer_malloc(sizeof(PKeyItem) * totalnum, 0);
	if(!cid->pkilist)
	{
		elog(ERROR, "Out of memery in add_space_for_cid");
	}
	if(0 != cid->malnum)
	{
		memcpy(cid->pkilist, old_space, cid->malnum);
		walminer_free((char*)old_space, 0);
	}

	cid->malnum = totalnum;
	walminer_debug("[add_space_for_cid]cid->malnum=%d,", cid->malnum);
}

static void
add_pkitem(ConstrintInDic *cid, HeapTuple tuple, Relation pgrel)
{
	Datum	conkeydatum;
	Datum	reliddatum;
	Datum	contypedatum;
	bool	isNull = false;
	int		numcols;
	int16	*attnums;
	Oid		reloid = InvalidOid;
	char	contype = 0;

	walminer_debug("[add_pkitem] foot");
	reliddatum = heap_getattr(tuple, Anum_pg_constraint_conrelid,
							  RelationGetDescr(pgrel), &isNull);
	contypedatum = heap_getattr(tuple, Anum_pg_constraint_contype,
							  RelationGetDescr(pgrel), &isNull);
	conkeydatum = heap_getattr(tuple, Anum_pg_constraint_conkey,
							  RelationGetDescr(pgrel), &isNull);
	reloid = DatumGetObjectId(reliddatum);
	contype = DatumGetChar(contypedatum);
	
	if (!isNull && contype == CONSTRAINT_PRIMARY)
	{
		ArrayType  *arr = NULL;

		arr = DatumGetArrayTypeP(conkeydatum);	/* ensure not toasted */
		numcols = ARR_DIMS(arr)[0];
		if (ARR_NDIM(arr) != 1 ||
			numcols < 0 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != INT2OID)
			elog(ERROR, "conkey is not a 1-D smallint array");
		attnums = (int16 *) ARR_DATA_PTR(arr);

		if(WALMINER_DICTIONARY_PRIMARY_KEY_MAX <= numcols)
		{
			elog(ERROR, "Can not support column number for a perkey up to 20 (reloid=%u)", reloid);
		}

		if(cid->malnum - 1 <= cid->indnum)
			add_space_for_cid(cid);
		
		cid->pkilist[cid->indnum].reloid = reloid;
		cid->pkilist[cid->indnum].keys = numcols;
		memcpy(cid->pkilist[cid->indnum].keynum, attnums, sizeof(int16) * numcols);
		cid->indnum++;
		walminer_debug("[add_pkitem]reloid=%u,numcols=%d,first=%d", reloid, numcols, attnums[0]);

	}
}

bool 
fetch_single_catalog(Oid reloid, int datasgsize, SingleCatalogCache *scc)
{
	bool				result = false;
	
	Relation			pgrel = NULL;
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	HeapScanDesc		scan = NULL;
#else
	SysScanDesc			scan = NULL;
#endif
	HeapTuple			tuple = NULL;

#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	pgrel = heap_open(reloid, AccessShareLock);
	if(!pgrel)
		ereport(ERROR,(errmsg("Open relation(oid:%d) failed",reloid)));
	scan = heap_beginscan_catalog(pgrel, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		add_tuple_to_cts(scc, tuple, datasgsize);
	}

	heap_endscan(scan);
	heap_close(pgrel, AccessShareLock);
#else
	pgrel = table_open(reloid, AccessShareLock);
	if(!pgrel)
		ereport(ERROR,(errmsg("Open relation(oid:%d) failed",reloid)));
	scan = systable_beginscan(pgrel, 0, false,
							   SnapshotSelf, 0, NULL);

	while ((tuple = systable_getnext(scan)) != NULL)
	{
		add_tuple_to_cts(scc, tuple, datasgsize);
	}

	systable_endscan(scan);
	table_close(pgrel, AccessShareLock);
#endif
	return result;
}

bool 
fetch_relation_pkey(ConstrintInDic *cid)
{
	bool				result = false;
	
	Relation			pgrel = NULL;
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	HeapScanDesc		scan = NULL;
#else
	SysScanDesc			scan = NULL;
#endif
	HeapTuple			tuple = NULL;
	Oid					reloid = ConstraintRelationId;

#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	pgrel = heap_open(reloid, AccessShareLock);
	if(!pgrel)
		ereport(ERROR,(errmsg("Open relation(oid:%d) failed",reloid)));
	scan = heap_beginscan_catalog(pgrel, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		add_pkitem(cid, tuple, pgrel);
	}

	heap_endscan(scan);
	heap_close(pgrel, AccessShareLock);
#else
	pgrel = table_open(reloid, AccessShareLock);
	if(!pgrel)
		ereport(ERROR,(errmsg("Open relation(oid:%d) failed",reloid)));
	scan = systable_beginscan(pgrel, 0, false,
							   SnapshotSelf, 0, NULL);

	while ((tuple = systable_getnext(scan)) != NULL)
	{
		add_pkitem(cid, tuple, pgrel);
	}

	systable_endscan(scan);
	table_close(pgrel, AccessShareLock);
#endif
	return result;
}


TupleDesc
makeOutputXlogDesc(void)
{
	TupleDesc tupdesc = NULL;
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	tupdesc = CreateTemplateTupleDesc(1, false);
#else
	tupdesc = CreateTemplateTupleDesc(1);
#endif
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "path", TEXTOID, -1, 0);
	return tupdesc;
}