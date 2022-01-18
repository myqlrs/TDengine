/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _TD_TSDB_MEM_TABLE_H_
#define _TD_TSDB_MEM_TABLE_H_

#include "tsdb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct STsdbMemTable STsdbMemTable;

STsdbMemTable *tsdbNewMemTable(STsdb *pTsdb);
void           tsdbFreeMemTable(STsdb *pTsdb, STsdbMemTable *pMemTable);
int            tsdbMemTableInsert(STsdb *pTsdb, STsdbMemTable *pMemTable, SSubmitMsg *pMsg, SShellSubmitRspMsg *pRsp);

#ifdef __cplusplus
}
#endif

#endif /*_TD_TSDB_MEM_TABLE_H_*/