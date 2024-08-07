/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wal2sql_ddl.c
 *
 *-------------------------------------------------------------------------
 */

#include "wal2sql_ddl.h"
#include "catalog/pg_class.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_depend.h"
#include "wm_utils.h"
#include "wal2sql.h"

static DDLData ddldata;

ReorderBufferChange *mchange = NULL;
TransactionEntry    *mte = NULL;


static void handle_catalog_for_ddl_trace(void);
static void handle_catalog_for_create_table(void);
static void handle_catalog_for_drop_table(void);
static void handle_catalog_for_add_table_column(void);
static void handle_catalog_for_alter_column_type(void);
static void handle_catalog_for_rewrite_table(void);

static void assemble_create_table(void);
static void assemble_drop_table(void);
static void assemble_truncate_table(void);
static void assemble_rename_table(void);
static void assemble_alter_table_add_column(AttItem *attribute_item);
static void assemble_drop_table_column(void);
static void assemble_alter_column_type(void);

static int* get_update_change_list(int *ccolnum);
static AttItem* get_first_attribute(void);
static void get_attribute_item(Form_pg_attribute fpa, AttItem *store_here);
static void get_pgclass_item(Form_pg_class fpc, bool new);
static void get_classname_by_attitem(Form_pg_attribute fpa);

#define IS_INSERT (REORDER_BUFFER_CHANGE_INSERT == mchange->action)
#define IS_UPDATE (REORDER_BUFFER_CHANGE_UPDATE == mchange->action)
#define IS_DELETE (REORDER_BUFFER_CHANGE_DELETE == mchange->action)

/*
 * 调用者必须保证ccollist已申请了足够的空间
 */
static int*
get_update_change_list(int *ccolnum)
{
    ReorderBufferTupleBuf   *tupbuf_new = NULL;
    ReorderBufferTupleBuf   *tupbuf_old = NULL;
    HeapTupleData           *tuple_new = NULL;
    HeapTupleData           *tuple_old = NULL;
    TupleDesc	            tupdesc = NULL;
    int                     natt = 0;
    int                     unsame_loop = 0;
    int                     *ccollist = NULL;
    ReorderBufferChange     *change = mchange;
    TransactionEntry        *te = mte; 

    tupdesc = te->tupdesc;
    ccollist = (int*)walminer_malloc(sizeof(int) * te->tupdesc->natts, 0);
    
    tupbuf_new = change->data.tp.newtuple;
    tupbuf_old = change->data.tp.oldtuple;

    Assert(tupbuf_new && tupbuf_old);
    tuple_new = &tupbuf_new->tuple;
    tuple_old = &tupbuf_old->tuple;

    for (natt = 0; natt < tupdesc->natts; natt++)
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

        if (attr->attisdropped)
			continue;
        typid = attr->atttypid;

        if(!get_typeoutput_fromdic(typid, &typoutput, &typisvarlena))
            elog(ERROR, "Can not find datatype %u", typid);

        neworigval = heap_getattr(tuple_new, natt + 1, tupdesc, &newisnull);
        oldorigval = heap_getattr(tuple_old, natt + 1, tupdesc, &oldisnull);

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

        if(!same)
        {
            ccollist[unsame_loop++] = natt;
        }
    }
    *ccolnum = unsame_loop;

    return ccollist;
}

static AttItem*
get_first_attribute(void)
{
    AttItem         *attribute_item  = NULL;
    ListCell        *cell = NULL;

    Assert(ddldata.attList);
    cell = list_head(ddldata.attList);

    attribute_item = (AttItem*)lfirst(cell);

    return attribute_item;
}

static void*
get_tuple_from_change(bool new)
{
    void    *result = NULL;

    if(new)
    {
        if(mchange->data.tp.newtuple)
            result = GETSTRUCT(&mchange->data.tp.newtuple->tuple);
    }
    else
    {
        if(mchange->data.tp.oldtuple)
            result = GETSTRUCT(&mchange->data.tp.oldtuple->tuple);
    }

    Assert(result);

    return result;
}

static void
get_attribute_item(Form_pg_attribute fpa, AttItem *store_here)
{
    AttItem             *att = NULL;

    if(store_here)
        memset(store_here, 0, sizeof(AttItem));
    if(0 < fpa->attnum)
    {
        char    *typname = NULL;

        att = (AttItem*)walminer_malloc(sizeof(AttItem), 0);
        memcpy(att->attName.data, fpa->attname.data, sizeof(NameData));
        att->attnum = fpa->attnum; 
        att->attKindOid = fpa->atttypid;
        att->attRelOid = fpa->attrelid;
        typname = get_typname_by_typoid(att->attKindOid);
        if(!typname)
        {
            typname = "(UNSURE)";
        }

        strncpy(att->addKindName.data, typname, sizeof(NameData));
        if(store_here)
        {
            memcpy(store_here, att, sizeof(AttItem));
            walminer_free((char*)att, 0);
        }
        else
            ddldata.attList = lappend(ddldata.attList, att);
    }
}

static void
get_classname_by_attitem(Form_pg_attribute fpa)
{
    Oid nspoid = 0;

    if(!get_relname_by_reloid(fpa->attrelid, &ddldata.relName))
    {
        strcpy(ddldata.relName.data, "(UNKNOWN_TABLE_NAME)");
    }
    if(get_nspoid_by_reloid(fpa->attrelid, &nspoid))
    {
        if(!get_nsp_by_nspoid(nspoid, &ddldata.nspName))
        {
            elog(ERROR, "Can not find nspoid %u in data dictionary", nspoid);
        }
    }
}

static void
get_pgclass_item(Form_pg_class fpc, bool new)
{
    ddldata.relpersistence = fpc->relpersistence;
#if (defined PG_VERSION_10) || (defined PG_VERSION_11)
    if(new)
        ddldata.reloid = HeapTupleGetOid(&mchange->data.tp.newtuple->tuple);
    else
        ddldata.reloid = HeapTupleGetOid(&mchange->data.tp.oldtuple->tuple);
#else
    ddldata.reloid = fpc->oid;
#endif
    memcpy(ddldata.relName.data, fpc->relname.data, sizeof(NameData));
    if(!get_nsp_by_nspoid(fpc->relnamespace, &ddldata.nspName))
    {
        elog(ERROR, "Can not find nspoid %u in data dictionary", fpc->relnamespace);
    }
    walminer_debug("[get_pgclass_item] ddldata.reloid=%u", ddldata.reloid);
}

void
init_ddl_analyse(void)
{
    memset(&ddldata, 0, sizeof(DDLData));
}

static bool
handle_insert_catalog(void)
{
    bool    traced = false;
    if(RelationRelationId == mte->reloid)
    {
        Form_pg_class fpc = NULL;

        fpc = (Form_pg_class)get_tuple_from_change(true);
        walminer_debug("[handle_insert_catalog] insert fpc->relkind=%d,%d", fpc->relkind, RELKIND_RELATION);
        /* 对pg_class插入了一个RELKIND_RELATION，则为create table */
        if(RELKIND_RELATION == fpc->relkind)
        {
            ddldata.ddlKind = DDLNO_TABLE_CREATE;
            get_pgclass_item(fpc,true);
            traced = true;
        }
    }
    else if(AttributeRelationId == mte->reloid)
    {
        
        get_attribute_item((Form_pg_attribute)get_tuple_from_change(true), NULL);
        ddldata.ddlKind = DDLNO_TABLE_ADD_COLUMN;
        traced = true;
    }

    return traced;
}

static bool
handle_delete_catalog(void)
{
    bool    traced = false;

    if(RelationRelationId == mte->reloid)
    {
        Form_pg_class fpc = NULL;

        fpc = (Form_pg_class)get_tuple_from_change(false);
        walminer_debug("[handle_delete_catalog] delete fpc->relkind=%d,%d", fpc->relkind, RELKIND_RELATION);
        /* 对pg_class删除了一个RELKIND_RELATION，则为drop table */
        if(RELKIND_RELATION == fpc->relkind)
        {
            ddldata.ddlKind = DDLNO_TABLE_DROP;
            get_pgclass_item(fpc, false);

            traced = true;
        }
    }

    return traced;
}

static bool
handle_update_catalog(void)
{
    bool    traced = false;
    int     *c_col_list = NULL;
    int     c_col_num = 0;

    if(RelationRelationId == mte->reloid)
    {
        Form_pg_class fpc_new = NULL;
        Form_pg_class fpc_old = NULL;

        fpc_old = (Form_pg_class)get_tuple_from_change(false);
        fpc_new = (Form_pg_class)get_tuple_from_change(true);
        walminer_debug("[handle_update_catalog] update fpc_old->relkind=%d,%d", fpc_old->relkind, RELKIND_RELATION);
        if(RELKIND_RELATION == fpc_old->relkind)
        {
            c_col_list = get_update_change_list(&c_col_num);

            if(number_in_array(c_col_list, c_col_num, Anum_pg_class_relfilenode - 1))
            {
                get_pgclass_item(fpc_old, false);
                walminer_debug("[handle_update_catalog] DDLNO_TABLE_TRUNCATE");
                ddldata.ddlKind = DDLNO_TABLE_TRUNCATE;
                traced = true;
            }
            else if(number_in_array(c_col_list, c_col_num, Anum_pg_class_relname - 1))
            {
                memcpy(ddldata.relName_new.data, fpc_new->relname.data, sizeof(NameData));
                get_pgclass_item(fpc_old, false);
                walminer_debug("[handle_update_catalog] DDLNO_TABLE_RENAME");
                ddldata.ddlKind = DDLNO_TABLE_RENAME;
                traced = true;
            }
        }
    }
    else if(AttributeRelationId == mte->reloid)
    {
        Form_pg_attribute   fpa_new = NULL;
        Form_pg_attribute   fpa_old = NULL;
        walminer_debug("[handle_update_catalog] update AttributeRelationId");

        c_col_list = get_update_change_list(&c_col_num);
        if(number_in_array(c_col_list, c_col_num, Anum_pg_attribute_attname - 1) &&
            number_in_array(c_col_list, c_col_num, Anum_pg_attribute_atttypid - 1) &&
            number_in_array(c_col_list, c_col_num, Anum_pg_attribute_attstattarget - 1) &&
            number_in_array(c_col_list, c_col_num, Anum_pg_attribute_attisdropped - 1))
        {
            fpa_old = (Form_pg_attribute)get_tuple_from_change(false);

            get_attribute_item(fpa_old, NULL);
            get_classname_by_attitem(fpa_old);
            ddldata.ddlKind = DDLNO_TABLE_DROP_COLUMN;
            traced = true;
        }
        else if(number_in_array(c_col_list, c_col_num, Anum_pg_attribute_atttypid - 1))
        {
            fpa_new = (Form_pg_attribute)get_tuple_from_change(true);
            ddldata.ddlKind = DDLNO_TABLE_ALTER_COLUMN_TYPE;
            get_attribute_item(fpa_new, NULL);
            ddldata.reloid = fpa_new->attrelid;
            Assert(0 != fpa_new->atttypid);

            traced = true;
        }
        else if(number_in_array(c_col_list, c_col_num, Anum_pg_attribute_attname - 1))
        {
            fpa_new = (Form_pg_attribute)get_tuple_from_change(true);
            fpa_old = (Form_pg_attribute)get_tuple_from_change(false);
            ddldata.ddlKind = DDLNO_TABLE_ALTER_COLUMN_RENAME;
            get_attribute_item(fpa_old, NULL);
            get_attribute_item(fpa_new, &ddldata.new_att);
            get_classname_by_attitem(fpa_old);

            traced = true;
        }


    }

    if(c_col_list)
        walminer_free((char*)c_col_list, 0);

    return traced;
}

/*
 * 建表语句，目前解析策略：
 * 入口为向pg_class插入一个RELKIND_RELATION类型的tuple
 * 需要记录向pg_attribute插入的各个列属性
 * 出口为向pg_depend插入的元组（classid=pg_class,objid=curoid）
 */
static void
handle_catalog_for_create_table(void)
{
    walminer_debug("[handle_catalog_for_create_table] foot");
    if(IS_INSERT)
    {
        walminer_debug("[handle_catalog_for_create_table] mte->reloid=%d,%d,%d",
                                    mte->reloid,AttributeRelationId, DependRelationId);
        if(AttributeRelationId == mte->reloid)
        {
            get_attribute_item((Form_pg_attribute)get_tuple_from_change(true), NULL);
        }
        else if(DependRelationId == mte->reloid)
        {
            Form_pg_depend  fpd = NULL;

            fpd = (Form_pg_depend)get_tuple_from_change(true);

            walminer_debug("[handle_catalog_for_create_table] fpd->objid=%u,%u",fpd->objid, ddldata.reloid);
            if(fpd->objid == ddldata.reloid && RelationRelationId == fpd->classid)
            {
                assemble_create_table();
                list_free_deep(ddldata.attList);
            }

        }
    }
}

/*
 * DROP表语句，目前解析策略：
 * 入口为删除pg_class的一个RELKIND_RELATION类型的tuple
 * 出口为删除pg_depend的元组（classid=pg_class,objid=curoid）
 */
static void
handle_catalog_for_drop_table(void)
{
    walminer_debug("[handle_catalog_for_drop_table] foot");
    if(IS_DELETE)
    {
        walminer_debug("[handle_catalog_for_drop_table] mte->reloid=%d,%d,%d",
                                    mte->reloid,AttributeRelationId, DependRelationId);
        if(DependRelationId == mte->reloid)
        {
            Form_pg_depend  fpd = NULL;

            fpd = (Form_pg_depend)get_tuple_from_change(false);

            walminer_debug("[handle_catalog_for_drop_table] fpd->objid=%u,%u",fpd->objid, ddldata.reloid);
            if(fpd->objid == ddldata.reloid && RelationRelationId == fpd->classid)
            {
                assemble_drop_table();
            }

        }
    }
}

/*
 * 表增列语句，目前解析策略：
 * 入口为pg_attribute插入数据
 * 出口为更新这个表的pg_class里的relnatts字段
 */
static void
handle_catalog_for_add_table_column(void)
{
    if(IS_UPDATE)
    {
        if(RelationRelationId == mte->reloid)
        {
            int             *c_col_list = NULL;
            int             c_col_num = 0;
            Form_pg_class   fpc_old = NULL;
            AttItem         *attribute_item  = NULL;
            
            Oid             reloid = 0;

            fpc_old = (Form_pg_class)get_tuple_from_change(false);
            
            attribute_item = get_first_attribute();

            c_col_list = get_update_change_list(&c_col_num);

#if (defined PG_VERSION_10) || (defined PG_VERSION_11)
            reloid = HeapTupleGetOid(&mchange->data.tp.oldtuple->tuple);
#else
            reloid = fpc_old->oid;
#endif

            if(number_in_array(c_col_list, c_col_num, Anum_pg_class_relnatts - 1) &&
                reloid == attribute_item->attRelOid)
            {

                ddldata.reloid = reloid;
                memcpy(ddldata.relName.data, fpc_old->relname.data, sizeof(NameData));
                if(!get_nsp_by_nspoid(fpc_old->relnamespace, &ddldata.nspName))
                {
                    elog(ERROR, "Can not find nspoid %u in data dictionary", fpc_old->relnamespace);
                }
                assemble_alter_table_add_column(attribute_item);
            }
            walminer_free((char*)c_col_list, 0);
        }
    }
}

/*
 * 表重写的匹配
 * 调用此函数需要保证:
 * 1. ddldata.relid已赋值
 * 2. 进入的全是对pg_class的处理
 */
static void
handle_catalog_for_rewrite_table(void)
{
    Form_pg_class   fpc_new = NULL;
    Form_pg_class   fpc_old = NULL;
    Oid             reloid = 0;
    int             *c_col_list = NULL;
    int             c_col_num = 0;

    if(1 == ddldata.rewrite_step && IS_UPDATE)
    {
        //等待pg_class更新为新的relfilenode

        fpc_new = (Form_pg_class)get_tuple_from_change(true);
        fpc_old = (Form_pg_class)get_tuple_from_change(false);
#if (defined PG_VERSION_10) || (defined PG_VERSION_11)
        reloid = HeapTupleGetOid(&mchange->data.tp.oldtuple->tuple);
#else
        reloid = fpc_old->oid;
#endif

        c_col_list = get_update_change_list(&c_col_num);
        if(reloid == ddldata.reloid &&
            number_in_array(c_col_list, c_col_num, Anum_pg_class_relfilenode - 1))
        {
            walminer_debug("[handle_catalog_for_rewrite_table] get new relfilenode");

            get_pgclass_item(fpc_old, false);
            ddldata.new_relfile_node = fpc_new->relfilenode;
            ddldata.rewrite_step = 2;
        }
    }
    else if(2 == ddldata.rewrite_step && IS_DELETE)
    {
        fpc_old = (Form_pg_class)get_tuple_from_change(false);
#if (defined PG_VERSION_10) || (defined PG_VERSION_11)
        reloid = HeapTupleGetOid(&mchange->data.tp.oldtuple->tuple);
#else
        reloid = fpc_old->oid;
#endif
        if(reloid == ddldata.new_relfile_node)//新创建的表relfilenode和oid相同
        {
            ddldata.rewrite_step = 3;
        }
    }
}

/*
 * 表列类型修改语句，目前解析策略：
 * 入口为pg_attribute的更新语句
 * 出口为表重写语句组
 */
static void
handle_catalog_for_alter_column_type(void)
{
    Assert(DDLNO_TABLE_ALTER_COLUMN_TYPE == ddldata.ddlKind);
    
    if(RelationRelationId != mte->reloid)
        return;
    if(0 == ddldata.rewrite_step)
        ddldata.rewrite_step = 1;
    
    handle_catalog_for_rewrite_table();

    if(3 == ddldata.rewrite_step)
        assemble_alter_column_type();
}




/* 组装DDL代码区域 */
static void
assemble_create_table(void)
{
    StringInfo  ddl = NULL;
    int         attlength = 0;
    ListCell    *cell = NULL;
    AttItem     *att = NULL;
    int         loop = 0;

    ddl = &mte->sql.notsql;
    attlength = ddldata.attList->length;

    appendStringInfo(ddl, "CREATE TABLE %s.%s(",  ddldata.nspName.data, ddldata.relName.data);
    foreach(cell, ddldata.attList)
    {
        att = lfirst(cell);
        if(loop == attlength - 1)
            appendStringInfo(ddl, "%s %s", att->attName.data, att->addKindName.data);
        else
            appendStringInfo(ddl, "%s %s,", att->attName.data, att->addKindName.data);
        loop++;
    }
    appendStringInfoString(ddl, ")");

    if(ddldata.attList)
        list_free_deep(ddldata.attList);
        

    mte->sqlkind = DECODE_SQL_KIND_DDL;
    mte->cananalyse = true;
    mte->sql.isddl = true;
    walminer_debug("[assemble_create_table]%s", ddl->data);
    insert_walcontents_tuple(mte);
    init_ddl_analyse();
}

static void
assemble_drop_table(void)
{
    StringInfo  ddl = NULL;

    ddl = &mte->sql.notsql;

    appendStringInfo(ddl, "DROP TABLE %s.%s",  ddldata.nspName.data, ddldata.relName.data);

    mte->sqlkind = DECODE_SQL_KIND_DDL;
    mte->cananalyse = true;
    mte->sql.isddl = true;
    walminer_debug("[assemble_drop_table]%s", ddl->data);
    insert_walcontents_tuple(mte);
    init_ddl_analyse();
}

static void
assemble_alter_table_add_column(AttItem *attribute_item)
{
    StringInfo  ddl = NULL;

    ddl = &mte->sql.notsql;

    appendStringInfo(ddl, "ALTER TABLE %s.%s ADD COLUMN %s %s",
                    ddldata.nspName.data, ddldata.relName.data,
                    attribute_item->attName.data, attribute_item->addKindName.data);

    mte->sqlkind = DECODE_SQL_KIND_DDL;
    mte->cananalyse = true;
    mte->sql.isddl = true;
    walminer_debug("[assemble_alter_table_add_column]%s", ddl->data);
    insert_walcontents_tuple(mte);
    init_ddl_analyse();
}

static void
assemble_truncate_table(void)
{
    StringInfo  ddl = NULL;

    ddl = &mte->sql.notsql;

    appendStringInfo(ddl, "TRUNCATE TABLE %s.%s",  ddldata.nspName.data, ddldata.relName.data);

    mte->sqlkind = DECODE_SQL_KIND_DDL;
    mte->cananalyse = true;
    mte->sql.isddl = true;
    walminer_debug("[assemble_truncate_table]%s", ddl->data);
    insert_walcontents_tuple(mte);
    init_ddl_analyse();
}

static void
assemble_rename_table(void)
{
    StringInfo  ddl = NULL;

    ddl = &mte->sql.notsql;

    appendStringInfo(ddl, "ALTER TABLE %s.%s RENAME TO %s",  ddldata.nspName.data, ddldata.relName.data,
                                                            ddldata.relName_new.data);

    mte->sqlkind = DECODE_SQL_KIND_DDL;
    mte->cananalyse = true;
    mte->sql.isddl = true;
    walminer_debug("[assemble_rename_table]%s", ddl->data);
    insert_walcontents_tuple(mte);
    init_ddl_analyse();
}

static void
assemble_drop_table_column(void)
{
    StringInfo  ddl = NULL;
    AttItem     *attribute_item = NULL;

    ddl = &mte->sql.notsql;

    attribute_item = get_first_attribute();

    if(0 < strlen(ddldata.nspName.data))
        appendStringInfo(ddl, "ALTER TABLE %s.%s DROP COLUMN %s",  ddldata.nspName.data, ddldata.relName.data,
                                                            attribute_item->attName.data);
    else
        appendStringInfo(ddl, "ALTER TABLE %s DROP COLUMN %s", ddldata.relName.data,
                                                            attribute_item->attName.data);

    mte->sqlkind = DECODE_SQL_KIND_DDL;
    mte->cananalyse = true;
    mte->sql.isddl = true;
    walminer_debug("[assemble_drop_table_column]%s", ddl->data);
    insert_walcontents_tuple(mte);
    init_ddl_analyse();
}

static void
assemble_alter_column_rename(void)
{
    StringInfo  ddl = NULL;
    AttItem     *attribute_item_old = NULL;
    AttItem     *attribute_item_new = NULL;

    ddl = &mte->sql.notsql;

    attribute_item_old = get_first_attribute();
    attribute_item_new = &ddldata.new_att;

    if(0 < strlen(ddldata.nspName.data))
        appendStringInfo(ddl, "ALTER TABLE %s.%s RENAME COLUMN %s TO %s",  ddldata.nspName.data, ddldata.relName.data,
                                                    attribute_item_old->attName.data, attribute_item_new->attName.data);
    else
        appendStringInfo(ddl, "ALTER TABLE %s RENAME COLUMN %s TO %s", ddldata.relName.data,
                                                    attribute_item_old->attName.data, attribute_item_new->attName.data);

    mte->sqlkind = DECODE_SQL_KIND_DDL;
    mte->cananalyse = true;
    mte->sql.isddl = true;
    walminer_debug("[assemble_alter_column_rename]%s", ddl->data);
    insert_walcontents_tuple(mte);
    init_ddl_analyse();
}

static void
assemble_alter_column_type(void)
{
    StringInfo  ddl = NULL;
    AttItem*    attribute_item = NULL;

    ddl = &mte->sql.notsql;

    attribute_item = get_first_attribute();

    appendStringInfo(ddl, "ALTER TABLE %s.%s ALTER COLUMN %s type %s",  ddldata.nspName.data, ddldata.relName.data,
                                                            attribute_item->attName.data, attribute_item->addKindName.data);

    mte->sqlkind = DECODE_SQL_KIND_DDL;
    mte->cananalyse = true;
    mte->sql.isddl = true;
    walminer_debug("[assemble_alter_column_type]%s", ddl->data);
    insert_walcontents_tuple(mte);
    init_ddl_analyse();
}

static void
handle_catalog_for_ddl_trace(void)
{
    bool    traced = false;

    Assert(IS_INSERT || IS_UPDATE || IS_DELETE);
    walminer_debug("[handle_catalog_for_ddl_trace] foot");
    if(IS_INSERT)
    {
        traced = handle_insert_catalog();
    }
    else if(IS_DELETE)
    {
       traced = handle_delete_catalog();
    }
    else if(IS_UPDATE)
    {
       traced = handle_update_catalog();
    }
    walminer_debug("[handle_catalog_for_ddl_trace]traced=%d", traced);
    ddldata.inddl = traced;
}

/*
 * DDL解析时，处理每一个系统表修改的入口函数。
 * 处理思路为，当前没有在ddl块里，那么需要使用handle_catalog_for_ddl_trace()
 * 函数，判断当前是否为一个目前支持的DDL特征修改。如果当前在ddl块里，那么需要
 * 根据不同的ddl块，分流处理这个系统表修改。
 * 
 * 注：目前遵从简单策略，后续会改动
 */
void
ddl_handle(ReorderBufferChange *change, TransactionEntry *te)
{
    mchange = change;
    mte = te;

    if(!ddldata.inddl)
    {
        walminer_debug("[ddl_handle]not inddl");
        handle_catalog_for_ddl_trace();
    }
    else
    {
        walminer_debug("[ddl_handle]ddldata.ddlKind=%d", ddldata.ddlKind);
        switch(ddldata.ddlKind)
        {
            case DDLNO_TABLE_CREATE:
                handle_catalog_for_create_table();
                break;
            case DDLNO_TABLE_DROP:
                handle_catalog_for_drop_table();
                break;

            case DDLNO_TABLE_ADD_COLUMN:
                handle_catalog_for_add_table_column();
                break;
            case DDLNO_TABLE_ALTER_COLUMN_TYPE:
                handle_catalog_for_alter_column_type();
                break;
            default:
                break;
        }
    }

    if(ddldata.inddl)
    {
        switch(ddldata.ddlKind)
        {
            case DDLNO_TABLE_TRUNCATE:
                assemble_truncate_table();
                break;
            case DDLNO_TABLE_RENAME:
                assemble_rename_table();
                break;
            case DDLNO_TABLE_DROP_COLUMN:
                assemble_drop_table_column();
                break;
            case DDLNO_TABLE_ALTER_COLUMN_RENAME:
                assemble_alter_column_rename();
                break;
            default:
                break;
        }
    }
}