/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wal2sql_ddl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALMINER_DDL_H
#define WALMINER_DDL_H

#include "walminer_decode.h"
#include "replication/reorderbuffer.h"

typedef enum DDLKind
{
    DDLNO_TABLE_CREATE = 0,
    DDLNO_TABLE_DROP,
    DDLNO_TABLE_TRUNCATE,

    DDLNO_TABLE_RENAME,
    DDLNO_TABLE_ADD_COLUMN,
    DDLNO_TABLE_DROP_COLUMN,
    DDLNO_TABLE_ALTER_COLUMN_TYPE,
    DDLNO_TABLE_ALTER_COLUMN_RENAME,

    DDLNO_INDEX_CREATE,
    DDLNO_INDEX_DROP,

    DDLNO_MAX
}DDLKind;

typedef struct AttItem
{
    NameData    attName;
    NameData    addKindName;
    Oid         attKindOid;
    Oid         attRelOid;
    int         attnum;
}AttItem;

typedef struct DDLData
{
    DDLKind     ddlKind;
    bool        inddl;
    /* 表相关 */
    NameData    nspName;
    NameData    relName;
    Oid         reloid;
    char        relpersistence;
    List        *attList;

    /* 表rename */
    NameData    nspName_new;
    NameData    relName_new;

    /* 表重写 */
    Oid         new_relfile_node;
    int         rewrite_step; //1=正在获取新relfilenode  2=正在删除旧relfilenode 3=完成

    /* 表列重名 */
    AttItem     new_att;

}DDLData;

void init_ddl_analyse(void);
void ddl_handle(ReorderBufferChange *change, TransactionEntry *te);

#endif