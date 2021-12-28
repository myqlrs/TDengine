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

#define _DEFAULT_SOURCE
#include "dndDnode.h"
#include "dndTransport.h"
#include "dndVnodes.h"
#include "dndMnode.h"

static int32_t dndInitMgmtWorker(SDnode *pDnode);
static void    dndCleanupMgmtWorker(SDnode *pDnode);
static int32_t dndAllocMgmtQueue(SDnode *pDnode);
static void    dndFreeMgmtQueue(SDnode *pDnode);
static void    dndProcessMgmtQueue(SDnode *pDnode, SRpcMsg *pMsg);

static int32_t dndReadDnodes(SDnode *pDnode);
static int32_t dndWriteDnodes(SDnode *pDnode);
static void   *dnodeThreadRoutine(void *param);

static int32_t dndProcessConfigDnodeReq(SDnode *pDnode, SRpcMsg *pMsg);
static void    dndProcessStatusRsp(SDnode *pDnode, SRpcMsg *pMsg);
static void    dndProcessAuthRsp(SDnode *pDnode, SRpcMsg *pMsg);
static void    dndProcessGrantRsp(SDnode *pDnode, SRpcMsg *pMsg);

int32_t dndGetDnodeId(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosRLockLatch(&pMgmt->latch);
  int32_t dnodeId = pMgmt->dnodeId;
  taosRUnLockLatch(&pMgmt->latch);
  return dnodeId;
}

int64_t dndGetClusterId(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosRLockLatch(&pMgmt->latch);
  int64_t clusterId = pMgmt->clusterId;
  taosRUnLockLatch(&pMgmt->latch);
  return clusterId;
}

void dndGetDnodeEp(SDnode *pDnode, int32_t dnodeId, char *pEp, char *pFqdn, uint16_t *pPort) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosRLockLatch(&pMgmt->latch);

  SDnodeEp *pDnodeEp = taosHashGet(pMgmt->dnodeHash, &dnodeId, sizeof(int32_t));
  if (pDnodeEp != NULL) {
    if (pPort != NULL) {
      *pPort = pDnodeEp->port;
    }
    if (pFqdn != NULL) {
      tstrncpy(pFqdn, pDnodeEp->fqdn, TSDB_FQDN_LEN);
    }
    if (pEp != NULL) {
      snprintf(pEp, TSDB_EP_LEN, "%s:%u", pDnodeEp->fqdn, pDnodeEp->port);
    }
  }

  taosRUnLockLatch(&pMgmt->latch);
}

void dndGetMnodeEpSet(SDnode *pDnode, SEpSet *pEpSet) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosRLockLatch(&pMgmt->latch);
  *pEpSet = pMgmt->mnodeEpSet;
  taosRUnLockLatch(&pMgmt->latch);
}

void dndSendRedirectMsg(SDnode *pDnode, SRpcMsg *pMsg) {
  tmsg_t msgType = pMsg->msgType;

  SEpSet epSet = {0};
  dndGetMnodeEpSet(pDnode, &epSet);

  dDebug("RPC %p, msg:%s is redirected, num:%d use:%d", pMsg->handle, TMSG_INFO(msgType), epSet.numOfEps, epSet.inUse);
  for (int32_t i = 0; i < epSet.numOfEps; ++i) {
    dDebug("mnode index:%d %s:%u", i, epSet.fqdn[i], epSet.port[i]);
    if (strcmp(epSet.fqdn[i], pDnode->opt.localFqdn) == 0 && epSet.port[i] == pDnode->opt.serverPort) {
      epSet.inUse = (i + 1) % epSet.numOfEps;
    }

    epSet.port[i] = htons(epSet.port[i]);
  }

  rpcSendRedirectRsp(pMsg->handle, &epSet);
}

static void dndUpdateMnodeEpSet(SDnode *pDnode, SEpSet *pEpSet) {
  dInfo("mnode is changed, num:%d use:%d", pEpSet->numOfEps, pEpSet->inUse);

  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosWLockLatch(&pMgmt->latch);

  pMgmt->mnodeEpSet = *pEpSet;
  for (int32_t i = 0; i < pEpSet->numOfEps; ++i) {
    dInfo("mnode index:%d %s:%u", i, pEpSet->fqdn[i], pEpSet->port[i]);
  }

  taosWUnLockLatch(&pMgmt->latch);
}

static void dndPrintDnodes(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  dDebug("print dnode ep list, num:%d", pMgmt->dnodeEps->num);
  for (int32_t i = 0; i < pMgmt->dnodeEps->num; i++) {
    SDnodeEp *pEp = &pMgmt->dnodeEps->eps[i];
    dDebug("dnode:%d, fqdn:%s port:%u isMnode:%d", pEp->id, pEp->fqdn, pEp->port, pEp->isMnode);
  }
}

static void dndResetDnodes(SDnode *pDnode, SDnodeEps *pDnodeEps) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  int32_t size = sizeof(SDnodeEps) + pDnodeEps->num * sizeof(SDnodeEp);
  if (pDnodeEps->num > pMgmt->dnodeEps->num) {
    SDnodeEps *tmp = calloc(1, size);
    if (tmp == NULL) return;

    tfree(pMgmt->dnodeEps);
    pMgmt->dnodeEps = tmp;
  }

  if (pMgmt->dnodeEps != pDnodeEps) {
    memcpy(pMgmt->dnodeEps, pDnodeEps, size);
  }

  pMgmt->mnodeEpSet.inUse = 0;
  pMgmt->mnodeEpSet.numOfEps = 0;

  int32_t mIndex = 0;
  for (int32_t i = 0; i < pMgmt->dnodeEps->num; i++) {
    SDnodeEp *pDnodeEp = &pMgmt->dnodeEps->eps[i];
    if (!pDnodeEp->isMnode) continue;
    if (mIndex >= TSDB_MAX_REPLICA) continue;
    pMgmt->mnodeEpSet.numOfEps++;
    strcpy(pMgmt->mnodeEpSet.fqdn[mIndex], pDnodeEp->fqdn);
    pMgmt->mnodeEpSet.port[mIndex] = pDnodeEp->port;
    mIndex++;
  }

  for (int32_t i = 0; i < pMgmt->dnodeEps->num; ++i) {
    SDnodeEp *pDnodeEp = &pMgmt->dnodeEps->eps[i];
    taosHashPut(pMgmt->dnodeHash, &pDnodeEp->id, sizeof(int32_t), pDnodeEp, sizeof(SDnodeEp));
  }

  dndPrintDnodes(pDnode);
}

static bool dndIsEpChanged(SDnode *pDnode, int32_t dnodeId, char *pEp) {
  bool changed = false;

  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosRLockLatch(&pMgmt->latch);

  SDnodeEp *pDnodeEp = taosHashGet(pMgmt->dnodeHash, &dnodeId, sizeof(int32_t));
  if (pDnodeEp != NULL) {
    char epstr[TSDB_EP_LEN + 1];
    snprintf(epstr, TSDB_EP_LEN, "%s:%u", pDnodeEp->fqdn, pDnodeEp->port);
    changed = strcmp(pEp, epstr) != 0;
  }

  taosRUnLockLatch(&pMgmt->latch);
  return changed;
}

static int32_t dndReadDnodes(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  int32_t code = TSDB_CODE_DND_DNODE_READ_FILE_ERROR;
  int32_t len = 0;
  int32_t maxLen = 256 * 1024;
  char   *content = calloc(1, maxLen + 1);
  cJSON  *root = NULL;
  FILE   *fp = NULL;

  fp = fopen(pMgmt->file, "r");
  if (fp == NULL) {
    dDebug("file %s not exist", pMgmt->file);
    code = 0;
    goto PRASE_DNODE_OVER;
  }

  len = (int32_t)fread(content, 1, maxLen, fp);
  if (len <= 0) {
    dError("failed to read %s since content is null", pMgmt->file);
    goto PRASE_DNODE_OVER;
  }

  content[len] = 0;
  root = cJSON_Parse(content);
  if (root == NULL) {
    dError("failed to read %s since invalid json format", pMgmt->file);
    goto PRASE_DNODE_OVER;
  }

  cJSON *dnodeId = cJSON_GetObjectItem(root, "dnodeId");
  if (!dnodeId || dnodeId->type != cJSON_Number) {
    dError("failed to read %s since dnodeId not found", pMgmt->file);
    goto PRASE_DNODE_OVER;
  }
  pMgmt->dnodeId = dnodeId->valueint;

  cJSON *clusterId = cJSON_GetObjectItem(root, "clusterId");
  if (!clusterId || clusterId->type != cJSON_String) {
    dError("failed to read %s since clusterId not found", pMgmt->file);
    goto PRASE_DNODE_OVER;
  }
  pMgmt->clusterId = atoll(clusterId->valuestring);

  cJSON *dropped = cJSON_GetObjectItem(root, "dropped");
  if (!dropped || dropped->type != cJSON_Number) {
    dError("failed to read %s since dropped not found", pMgmt->file);
    goto PRASE_DNODE_OVER;
  }
  pMgmt->dropped = dropped->valueint;

  cJSON *dnodes = cJSON_GetObjectItem(root, "dnodes");
  if (!dnodes || dnodes->type != cJSON_Array) {
    dError("failed to read %s since dnodes not found", pMgmt->file);
    goto PRASE_DNODE_OVER;
  }

  int32_t numOfDnodes = cJSON_GetArraySize(dnodes);
  if (numOfDnodes <= 0) {
    dError("failed to read %s since numOfDnodes:%d invalid", pMgmt->file, numOfDnodes);
    goto PRASE_DNODE_OVER;
  }

  pMgmt->dnodeEps = calloc(1, numOfDnodes * sizeof(SDnodeEp) + sizeof(SDnodeEps));
  if (pMgmt->dnodeEps == NULL) {
    dError("failed to calloc dnodeEpList since %s", strerror(errno));
    goto PRASE_DNODE_OVER;
  }
  pMgmt->dnodeEps->num = numOfDnodes;

  for (int32_t i = 0; i < numOfDnodes; ++i) {
    cJSON *node = cJSON_GetArrayItem(dnodes, i);
    if (node == NULL) break;

    SDnodeEp *pDnodeEp = &pMgmt->dnodeEps->eps[i];

    cJSON *dnodeId = cJSON_GetObjectItem(node, "id");
    if (!dnodeId || dnodeId->type != cJSON_Number) {
      dError("failed to read %s since dnodeId not found", pMgmt->file);
      goto PRASE_DNODE_OVER;
    }
    pDnodeEp->id = dnodeId->valueint;

    cJSON *dnodeFqdn = cJSON_GetObjectItem(node, "fqdn");
    if (!dnodeFqdn || dnodeFqdn->type != cJSON_String || dnodeFqdn->valuestring == NULL) {
      dError("failed to read %s since dnodeFqdn not found", pMgmt->file);
      goto PRASE_DNODE_OVER;
    }
    tstrncpy(pDnodeEp->fqdn, dnodeFqdn->valuestring, TSDB_FQDN_LEN);

    cJSON *dnodePort = cJSON_GetObjectItem(node, "port");
    if (!dnodePort || dnodePort->type != cJSON_Number) {
      dError("failed to read %s since dnodePort not found", pMgmt->file);
      goto PRASE_DNODE_OVER;
    }
    pDnodeEp->port = dnodePort->valueint;

    cJSON *isMnode = cJSON_GetObjectItem(node, "isMnode");
    if (!isMnode || isMnode->type != cJSON_Number) {
      dError("failed to read %s since isMnode not found", pMgmt->file);
      goto PRASE_DNODE_OVER;
    }
    pDnodeEp->isMnode = isMnode->valueint;
  }

  code = 0;
  dInfo("succcessed to read file %s", pMgmt->file);
  dndPrintDnodes(pDnode);

PRASE_DNODE_OVER:
  if (content != NULL) free(content);
  if (root != NULL) cJSON_Delete(root);
  if (fp != NULL) fclose(fp);

  if (dndIsEpChanged(pDnode, pMgmt->dnodeId, pDnode->opt.localEp)) {
    dError("localEp %s different with %s and need reconfigured", pDnode->opt.localEp, pMgmt->file);
    return -1;
  }

  if (pMgmt->dnodeEps == NULL) {
    pMgmt->dnodeEps = calloc(1, sizeof(SDnodeEps) + sizeof(SDnodeEp));
    pMgmt->dnodeEps->num = 1;
    pMgmt->dnodeEps->eps[0].isMnode = 1;
    taosGetFqdnPortFromEp(pDnode->opt.firstEp, pMgmt->dnodeEps->eps[0].fqdn, &pMgmt->dnodeEps->eps[0].port);
  }

  dndResetDnodes(pDnode, pMgmt->dnodeEps);

  terrno = 0;
  return 0;
}

static int32_t dndWriteDnodes(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  FILE *fp = fopen(pMgmt->file, "w");
  if (fp == NULL) {
    dError("failed to write %s since %s", pMgmt->file, strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  int32_t len = 0;
  int32_t maxLen = 256 * 1024;
  char   *content = calloc(1, maxLen + 1);

  len += snprintf(content + len, maxLen - len, "{\n");
  len += snprintf(content + len, maxLen - len, "  \"dnodeId\": %d,\n", pMgmt->dnodeId);
  len += snprintf(content + len, maxLen - len, "  \"clusterId\": \"%" PRId64 "\",\n", pMgmt->clusterId);
  len += snprintf(content + len, maxLen - len, "  \"dropped\": %d,\n", pMgmt->dropped);
  len += snprintf(content + len, maxLen - len, "  \"dnodes\": [{\n");
  for (int32_t i = 0; i < pMgmt->dnodeEps->num; ++i) {
    SDnodeEp *pDnodeEp = &pMgmt->dnodeEps->eps[i];
    len += snprintf(content + len, maxLen - len, "    \"id\": %d,\n", pDnodeEp->id);
    len += snprintf(content + len, maxLen - len, "    \"fqdn\": \"%s\",\n", pDnodeEp->fqdn);
    len += snprintf(content + len, maxLen - len, "    \"port\": %u,\n", pDnodeEp->port);
    len += snprintf(content + len, maxLen - len, "    \"isMnode\": %d\n", pDnodeEp->isMnode);
    if (i < pMgmt->dnodeEps->num - 1) {
      len += snprintf(content + len, maxLen - len, "  },{\n");
    } else {
      len += snprintf(content + len, maxLen - len, "  }]\n");
    }
  }
  len += snprintf(content + len, maxLen - len, "}\n");

  fwrite(content, 1, len, fp);
  taosFsyncFile(fileno(fp));
  fclose(fp);
  free(content);
  terrno = 0;

  pMgmt->updateTime = taosGetTimestampMs();
  dInfo("successed to write %s", pMgmt->file);
  return 0;
}

void dndSendStatusMsg(SDnode *pDnode) {
  int32_t contLen = sizeof(SStatusMsg) + TSDB_MAX_VNODES * sizeof(SVnodeLoad);

  SStatusMsg *pStatus = rpcMallocCont(contLen);
  if (pStatus == NULL) {
    dError("failed to malloc status message");
    return;
  }

  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosRLockLatch(&pMgmt->latch);
  pStatus->sver = htonl(pDnode->opt.sver);
  pStatus->dnodeId = htonl(pMgmt->dnodeId);
  pStatus->clusterId = htobe64(pMgmt->clusterId);
  pStatus->rebootTime = htobe64(pMgmt->rebootTime);
  pStatus->updateTime = htobe64(pMgmt->updateTime);
  pStatus->numOfCores = htons(pDnode->opt.numOfCores);
  pStatus->numOfSupportVnodes = htons(pDnode->opt.numOfSupportVnodes);
  tstrncpy(pStatus->dnodeEp, pDnode->opt.localEp, TSDB_EP_LEN);

  pStatus->clusterCfg.statusInterval = htonl(pDnode->opt.statusInterval);
  pStatus->clusterCfg.checkTime = 0;
  char timestr[32] = "1970-01-01 00:00:00.00";
  (void)taosParseTime(timestr, &pStatus->clusterCfg.checkTime, (int32_t)strlen(timestr), TSDB_TIME_PRECISION_MILLI, 0);
  pStatus->clusterCfg.checkTime = htonl(pStatus->clusterCfg.checkTime);
  tstrncpy(pStatus->clusterCfg.timezone, pDnode->opt.timezone, TSDB_TIMEZONE_LEN);
  tstrncpy(pStatus->clusterCfg.locale, pDnode->opt.locale, TSDB_LOCALE_LEN);
  tstrncpy(pStatus->clusterCfg.charset, pDnode->opt.charset, TSDB_LOCALE_LEN);
  taosRUnLockLatch(&pMgmt->latch);

  dndGetVnodeLoads(pDnode, &pStatus->vnodeLoads);
  contLen = sizeof(SStatusMsg) + pStatus->vnodeLoads.num * sizeof(SVnodeLoad);

  SRpcMsg rpcMsg = {.pCont = pStatus, .contLen = contLen, .msgType = TDMT_MND_STATUS, .ahandle = (void *)9527};
  pMgmt->statusSent = 1;

  dTrace("pDnode:%p, send status msg to mnode", pDnode);
  dndSendMsgToMnode(pDnode, &rpcMsg);
}

static void dndUpdateDnodeCfg(SDnode *pDnode, SDnodeCfg *pCfg) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  if (pMgmt->dnodeId == 0 || pMgmt->dropped != pCfg->dropped) {
    dInfo("set dnodeId:%d clusterId:% " PRId64 " dropped:%d", pCfg->dnodeId, pCfg->clusterId, pCfg->dropped);

    taosWLockLatch(&pMgmt->latch);
    pMgmt->dnodeId = pCfg->dnodeId;
    pMgmt->clusterId = pCfg->clusterId;
    pMgmt->dropped = pCfg->dropped;
    dndWriteDnodes(pDnode);
    taosWUnLockLatch(&pMgmt->latch);
  }
}

static void dndUpdateDnodeEps(SDnode *pDnode, SDnodeEps *pDnodeEps) {
  if (pDnodeEps == NULL || pDnodeEps->num <= 0) return;

  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosWLockLatch(&pMgmt->latch);

  if (pDnodeEps->num != pMgmt->dnodeEps->num) {
    dndResetDnodes(pDnode, pDnodeEps);
    dndWriteDnodes(pDnode);
  } else {
    int32_t size = pDnodeEps->num * sizeof(SDnodeEp) + sizeof(SDnodeEps);
    if (memcmp(pMgmt->dnodeEps, pDnodeEps, size) != 0) {
      dndResetDnodes(pDnode, pDnodeEps);
      dndWriteDnodes(pDnode);
    }
  }

  taosWUnLockLatch(&pMgmt->latch);
}

static void dndProcessStatusRsp(SDnode *pDnode, SRpcMsg *pMsg) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  if (pMsg->code != TSDB_CODE_SUCCESS) {
    pMgmt->statusSent = 0;
    return;
  }

  SStatusRsp *pRsp = pMsg->pCont;
  SDnodeCfg  *pCfg = &pRsp->dnodeCfg;
  pCfg->dnodeId = htonl(pCfg->dnodeId);
  pCfg->clusterId = htobe64(pCfg->clusterId);
  dndUpdateDnodeCfg(pDnode, pCfg);

  if (pCfg->dropped) {
    pMgmt->statusSent = 0;
    return;
  }

  SDnodeEps *pDnodeEps = &pRsp->dnodeEps;
  pDnodeEps->num = htonl(pDnodeEps->num);
  for (int32_t i = 0; i < pDnodeEps->num; ++i) {
    pDnodeEps->eps[i].id = htonl(pDnodeEps->eps[i].id);
    pDnodeEps->eps[i].port = htons(pDnodeEps->eps[i].port);
  }

  dndUpdateDnodeEps(pDnode, pDnodeEps);
  pMgmt->statusSent = 0;
}

static void dndProcessAuthRsp(SDnode *pDnode, SRpcMsg *pMsg) { assert(1); }

static void dndProcessGrantRsp(SDnode *pDnode, SRpcMsg *pMsg) { assert(1); }

static int32_t dndProcessConfigDnodeReq(SDnode *pDnode, SRpcMsg *pMsg) {
  dError("config msg is received, but not supported yet");
  SCfgDnodeMsg *pCfg = pMsg->pCont;

  return TSDB_CODE_OPS_NOT_SUPPORT;
}

void dndProcessStartupReq(SDnode *pDnode, SRpcMsg *pMsg) {
  dDebug("startup msg is received");

  SStartupMsg *pStartup = rpcMallocCont(sizeof(SStartupMsg));
  dndGetStartup(pDnode, pStartup);

  dDebug("startup msg is sent, step:%s desc:%s finished:%d", pStartup->name, pStartup->desc, pStartup->finished);

  SRpcMsg rpcRsp = {.handle = pMsg->handle, .pCont = pStartup, .contLen = sizeof(SStartupMsg)};
  rpcSendResponse(&rpcRsp);
}

static void *dnodeThreadRoutine(void *param) {
  SDnode     *pDnode = param;
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  int32_t     ms = pDnode->opt.statusInterval * 1000;

  while (true) {
    pthread_testcancel();
    taosMsleep(ms);

    if (dndGetStat(pDnode) == DND_STAT_RUNNING && !pMgmt->statusSent) {
      dndSendStatusMsg(pDnode);
    }
  }
}

int32_t dndInitDnode(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  pMgmt->dnodeId = 0;
  pMgmt->rebootTime = taosGetTimestampMs();
  pMgmt->dropped = 0;
  pMgmt->clusterId = 0;
  taosInitRWLatch(&pMgmt->latch);

  char path[PATH_MAX];
  snprintf(path, PATH_MAX, "%s/dnode.json", pDnode->dir.dnode);
  pMgmt->file = strdup(path);
  if (pMgmt->file == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  pMgmt->dnodeHash = taosHashInit(4, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), true, HASH_NO_LOCK);
  if (pMgmt->dnodeHash == NULL) {
    dError("failed to init dnode hash");
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  if (dndReadDnodes(pDnode) != 0) {
    dError("failed to read file:%s since %s", pMgmt->file, terrstr());
    return -1;
  }

  if (dndInitMgmtWorker(pDnode) != 0) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  if (dndAllocMgmtQueue(pDnode) != 0) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  pMgmt->threadId = taosCreateThread(dnodeThreadRoutine, pDnode);
  if (pMgmt->threadId == NULL) {
    dError("failed to init dnode thread");
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  dInfo("dnode-dnode is initialized");
  return 0;
}

void dndCleanupDnode(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  dndCleanupMgmtWorker(pDnode);
  dndFreeMgmtQueue(pDnode);

  if (pMgmt->threadId != NULL) {
    taosDestoryThread(pMgmt->threadId);
    pMgmt->threadId = NULL;
  }

  taosWLockLatch(&pMgmt->latch);

  if (pMgmt->dnodeEps != NULL) {
    free(pMgmt->dnodeEps);
    pMgmt->dnodeEps = NULL;
  }

  if (pMgmt->dnodeHash != NULL) {
    taosHashCleanup(pMgmt->dnodeHash);
    pMgmt->dnodeHash = NULL;
  }

  if (pMgmt->file != NULL) {
    free(pMgmt->file);
    pMgmt->file = NULL;
  }

  taosWUnLockLatch(&pMgmt->latch);
  dInfo("dnode-dnode is cleaned up");
}

static int32_t dndInitMgmtWorker(SDnode *pDnode) {
  SDnodeMgmt  *pMgmt = &pDnode->dmgmt;
  SWorkerPool *pPool = &pMgmt->mgmtPool;
  pPool->name = "dnode-mgmt";
  pPool->min = 1;
  pPool->max = 1;
  if (tWorkerInit(pPool) != 0) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  dDebug("dnode mgmt worker is initialized");
  return 0;
}

static void dndCleanupMgmtWorker(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  tWorkerCleanup(&pMgmt->mgmtPool);
  dDebug("dnode mgmt worker is closed");
}

static int32_t dndAllocMgmtQueue(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  pMgmt->pMgmtQ = tWorkerAllocQueue(&pMgmt->mgmtPool, pDnode, (FProcessItem)dndProcessMgmtQueue);
  if (pMgmt->pMgmtQ == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }
  return 0;
}

static void dndFreeMgmtQueue(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  tWorkerFreeQueue(&pMgmt->mgmtPool, pMgmt->pMgmtQ);
  pMgmt->pMgmtQ = NULL;
}

void dndProcessMgmtMsg(SDnode *pDnode, SRpcMsg *pRpcMsg, SEpSet *pEpSet) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  if (pEpSet && pEpSet->numOfEps > 0 && pRpcMsg->msgType == TDMT_MND_STATUS_RSP) {
    dndUpdateMnodeEpSet(pDnode, pEpSet);
  }

  SRpcMsg *pMsg = taosAllocateQitem(sizeof(SRpcMsg));
  if (pMsg != NULL) *pMsg = *pRpcMsg;

  if (pMsg == NULL || taosWriteQitem(pMgmt->pMgmtQ, pMsg) != 0) {
    if (pRpcMsg->msgType & 1u) {
      SRpcMsg rsp = {.handle = pRpcMsg->handle, .code = TSDB_CODE_OUT_OF_MEMORY};
      rpcSendResponse(&rsp);
    }
    rpcFreeCont(pRpcMsg->pCont);
    taosFreeQitem(pMsg);
  }
}

static void dndProcessMgmtQueue(SDnode *pDnode, SRpcMsg *pMsg) {
  int32_t code = 0;

  switch (pMsg->msgType) {
    case TDMT_DND_CREATE_MNODE:
      code = dndProcessCreateMnodeReq(pDnode, pMsg);
      break;
    case TDMT_DND_ALTER_MNODE:
      code = dndProcessAlterMnodeReq(pDnode, pMsg);
      break;
    case TDMT_DND_DROP_MNODE:
      code = dndProcessDropMnodeReq(pDnode, pMsg);
      break;
    case TDMT_DND_CONFIG_DNODE:
      code = dndProcessConfigDnodeReq(pDnode, pMsg);
      break;
    case TDMT_MND_STATUS_RSP:
      dndProcessStatusRsp(pDnode, pMsg);
      break;
    case TDMT_MND_AUTH_RSP:
      dndProcessAuthRsp(pDnode, pMsg);
      break;
    case TDMT_MND_GRANT_RSP:
      dndProcessGrantRsp(pDnode, pMsg);
      break;
    case TDMT_DND_CREATE_VNODE:
      code = dndProcessCreateVnodeReq(pDnode, pMsg);
      break;
    case TDMT_DND_ALTER_VNODE:
      code = dndProcessAlterVnodeReq(pDnode, pMsg);
      break;
    case TDMT_DND_DROP_VNODE:
      code = dndProcessDropVnodeReq(pDnode, pMsg);
      break;
    case TDMT_DND_AUTH_VNODE:
      code = dndProcessAuthVnodeReq(pDnode, pMsg);
      break;
    case TDMT_DND_SYNC_VNODE:
      code = dndProcessSyncVnodeReq(pDnode, pMsg);
      break;
    case TDMT_DND_COMPACT_VNODE:
      code = dndProcessCompactVnodeReq(pDnode, pMsg);
      break;
    default:
      terrno = TSDB_CODE_MSG_NOT_PROCESSED;
      code = -1;
      dError("RPC %p, dnode req:%s not processed", pMsg->handle, TMSG_INFO(pMsg->msgType));
      break;
  }

  if (pMsg->msgType & 1u) {
    if (code != 0) code = terrno;
    SRpcMsg rsp = {.code = code, .handle = pMsg->handle, .ahandle = pMsg->ahandle};
    rpcSendResponse(&rsp);
  }

  rpcFreeCont(pMsg->pCont);
  pMsg->pCont = NULL;
  taosFreeQitem(pMsg);
}