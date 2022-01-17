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

#ifndef _TD_MND_H_
#define _TD_MND_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------ TYPES EXPOSED ------------------------ */
typedef struct SDnode    SDnode;
typedef struct SMnode    SMnode;
typedef struct SMnodeMsg SMnodeMsg;
typedef int32_t (*SendReqToDnodeFp)(SDnode *pDnode, struct SEpSet *epSet, struct SRpcMsg *rpcMsg);
typedef int32_t (*SendReqToMnodeFp)(SDnode *pDnode, struct SRpcMsg *rpcMsg);
typedef int32_t (*PutReqToMWriteQFp)(SDnode *pDnode, struct SRpcMsg *rpcMsg);
typedef void (*SendRedirectRspFp)(SDnode *pDnode, struct SRpcMsg *rpcMsg);

typedef struct SMnodeLoad {
  int64_t numOfDnode;
  int64_t numOfMnode;
  int64_t numOfVgroup;
  int64_t numOfDatabase;
  int64_t numOfSuperTable;
  int64_t numOfChildTable;
  int64_t numOfNormalTable;
  int64_t numOfColumn;
  int64_t totalPoints;
  int64_t totalStorage;
  int64_t compStorage;
} SMnodeLoad;

typedef struct SMnodeCfg {
  int32_t sver;
  int8_t  enableTelem;
  int32_t statusInterval;
  int32_t shellActivityTimer;
  char   *timezone;
  char   *locale;
  char   *charset;
  char   *buildinfo;
  char   *gitinfo;
} SMnodeCfg;

typedef struct {
  int32_t           dnodeId;
  int64_t           clusterId;
  int8_t            replica;
  int8_t            selfIndex;
  SReplica          replicas[TSDB_MAX_REPLICA];
  SMnodeCfg         cfg;
  SDnode           *pDnode;
  PutReqToMWriteQFp putReqToMWriteQFp;
  SendReqToDnodeFp  sendReqToDnodeFp;
  SendReqToMnodeFp  sendReqToMnodeFp;
  SendRedirectRspFp sendRedirectRspFp;
} SMnodeOpt;

/* ------------------------ SMnode ------------------------ */
/**
 * @brief Open a mnode.
 *
 * @param path Path of the mnode.
 * @param pOption Option of the mnode.
 * @return SMnode* The mnode object.
 */
SMnode *mndOpen(const char *path, const SMnodeOpt *pOption);

/**
 * @brief Close a mnode.
 *
 * @param pMnode The mnode object to close.
 */
void mndClose(SMnode *pMnode);

/**
 * @brief Close a mnode.
 *
 * @param pMnode The mnode object to close.
 * @param pOption Options of the mnode.
 * @return int32_t 0 for success, -1 for failure.
 */
int32_t mndAlter(SMnode *pMnode, const SMnodeOpt *pOption);

/**
 * @brief Drop a mnode.
 *
 * @param path Path of the mnode.
 */
void mndDestroy(const char *path);

/**
 * @brief Get mnode statistics info.
 *
 * @param pMnode The mnode object.
 * @param pLoad Statistics of the mnode.
 * @return int32_t 0 for success, -1 for failure.
 */
int32_t mndGetLoad(SMnode *pMnode, SMnodeLoad *pLoad);

/**
 * @brief Get user authentication info.
 *
 * @param pMnode The mnode object.
 * @param user
 * @param spi
 * @param encrypt
 * @param secret
 * @param ckey
 * @return int32_t 0 for success, -1 for failure.
 */
int32_t mndRetriveAuth(SMnode *pMnode, char *user, char *spi, char *encrypt, char *secret, char *ckey);

/**
 * @brief Initialize mnode msg.
 *
 * @param pMnode The mnode object.
 * @param pMsg The request rpc msg.
 * @return int32_t The created mnode msg.
 */
SMnodeMsg *mndInitMsg(SMnode *pMnode, SRpcMsg *pRpcMsg);

/**
 * @brief Cleanup mnode msg.
 *
 * @param pMsg The request msg.
 */
void mndCleanupMsg(SMnodeMsg *pMsg);

/**
 * @brief Cleanup mnode msg.
 *
 * @param pMsg The request msg.
 * @param code The error code.
 */
void mndSendRsp(SMnodeMsg *pMsg, int32_t code);

/**
 * @brief Process the read, write, sync request.
 *
 * @param pMsg The request msg.
 * @return int32_t 0 for success, -1 for failure.
 */
void mndProcessMsg(SMnodeMsg *pMsg);

#ifdef __cplusplus
}
#endif

#endif /*_TD_MND_H_*/