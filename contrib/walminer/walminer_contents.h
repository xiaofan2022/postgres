/*-------------------------------------------------------------------------
 *
 * walminer_contents.h
 *
 *-------------------------------------------------------------------------
*/

#ifndef WALMINER_CONTENTS_H
#define WALMINER_CONTENTS_H
#include "postgres.h"

typedef struct FormData_walminer_contents
{
	uint32			sqlno;
	uint32			xid;
	uint32			topxid;
	int				sqlkind;
	bool			minerd;
	TimestampTz		timestamp;
	char*			op_text;
	bool			complete;
	char*			schema;
	char*			relation;
	XLogRecPtr		start_lsn;
	XLogRecPtr		commit_lsn;
}FormData_walminer_contents;

typedef FormData_walminer_contents *Form_walminer_contents;



#define Natts_walminer_contents									14
#define Anum_walminer_contents_no								1
#define Anum_walminer_contents_xid								2
#define Anum_walminer_contents_topxid							3
#define Anum_walminer_contents_sqlkind							4
#define Anum_walminer_contents_minerd							5
#define Anum_walminer_contents_timestamp						6
#define Anum_walminer_contents_op_text							7
#define Anum_walminer_contents_undo_text						8
#define Anum_walminer_contents_complete							9
#define Anum_walminer_contents_schema							10
#define Anum_walminer_contents_relation							11
#define Anum_walminer_contents_startlsn							12
#define Anum_walminer_contents_commitlsn						13

void insert_walcontents_tuple(void *te);

#endif
