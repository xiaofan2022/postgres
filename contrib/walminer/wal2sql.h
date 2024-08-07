/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wal2sql.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WAL2SQL_H
#define WAL2SQL_H

#include "walminer_decode.h"

void wal2sql_commit_single_transaction(TransactionEntry *te);

char*
convert_attr_to_str(Form_pg_attribute fpa,Oid typoutput, bool typisvarlena, Datum attr, List *toast_list);

#endif