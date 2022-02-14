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

#ifdef USE_UV

#include "transComm.h"

typedef struct SSrvConn {
  uv_tcp_t*   pTcp;
  uv_write_t* pWriter;
  uv_timer_t* pTimer;

  // uv_async_t* pWorkerAsync;
  queue       queue;
  int         ref;
  int         persist;  // persist connection or not
  SConnBuffer readBuf;  // read buf,
  int         inType;
  void*       pTransInst;  // rpc init
  void*       ahandle;     //
  void*       hostThrd;
  void*       pSrvMsg;

  struct sockaddr_in addr;

  // SRpcMsg sendMsg;
  // del later
  char secured;
  int  spi;
  char info[64];
  char user[TSDB_UNI_LEN];  // user ID for the link
  char secret[TSDB_PASSWORD_LEN];
  char ckey[TSDB_PASSWORD_LEN];  // ciphering key
} SSrvConn;

typedef struct SSrvMsg {
  SSrvConn* pConn;
  SRpcMsg   msg;
  queue     q;
} SSrvMsg;

typedef struct SWorkThrdObj {
  pthread_t   thread;
  uv_pipe_t*  pipe;
  int         fd;
  uv_loop_t*  loop;
  SAsyncPool* asyncPool;
  // uv_async_t*     workerAsync;  //
  queue           msg;
  pthread_mutex_t msgMtx;
  void*           pTransInst;
} SWorkThrdObj;

typedef struct SServerObj {
  pthread_t      thread;
  uv_tcp_t       server;
  uv_loop_t*     loop;
  int            workerIdx;
  int            numOfThreads;
  SWorkThrdObj** pThreadObj;
  uv_pipe_t**    pipe;
  uint32_t       ip;
  uint32_t       port;
  uv_async_t*    pAcceptAsync;  // just to quit from from accept thread
} SServerObj;

static const char* notify = "a";

// refactor later
static int transAddAuthPart(SSrvConn* pConn, char* msg, int msgLen);

static int uvAuthMsg(SSrvConn* pConn, char* msg, int msgLen);

static void uvAllocConnBufferCb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void uvAllocReadBufferCb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void uvOnReadCb(uv_stream_t* cli, ssize_t nread, const uv_buf_t* buf);
static void uvOnTimeoutCb(uv_timer_t* handle);
static void uvOnWriteCb(uv_write_t* req, int status);
static void uvOnPipeWriteCb(uv_write_t* req, int status);
static void uvOnAcceptCb(uv_stream_t* stream, int status);
static void uvOnConnectionCb(uv_stream_t* q, ssize_t nread, const uv_buf_t* buf);
static void uvWorkerAsyncCb(uv_async_t* handle);
static void uvAcceptAsyncCb(uv_async_t* handle);

static void uvPrepareSendData(SSrvMsg* msg, uv_buf_t* wb);
static void uvStartSendResp(SSrvMsg* msg);

static void destroySmsg(SSrvMsg* smsg);
// check whether already read complete packet
static bool      readComplete(SConnBuffer* buf);
static SSrvConn* createConn();
static void      destroyConn(SSrvConn* conn, bool clear /*clear handle or not*/);

static void uvDestroyConn(uv_handle_t* handle);

// server and worker thread
static void* workerThread(void* arg);
static void* acceptThread(void* arg);

// add handle loop
static bool addHandleToWorkloop(void* arg);
static bool addHandleToAcceptloop(void* arg);

void uvAllocReadBufferCb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  /*
   * formate of data buffer:
   * |<--------------------------data from socket------------------------------->|
   * |<------STransMsgHead------->|<-------------------other data--------------->|
   */
  SSrvConn*    conn = handle->data;
  SConnBuffer* pBuf = &conn->readBuf;
  transAllocBuffer(pBuf, buf);
}

// check data read from socket complete or not
//
static bool readComplete(SConnBuffer* data) {
  // TODO(yihao): handle pipeline later
  STransMsgHead head;
  int32_t       headLen = sizeof(head);
  if (data->len >= headLen) {
    memcpy((char*)&head, data->buf, headLen);
    int32_t msgLen = (int32_t)htonl((uint32_t)head.msgLen);
    if (msgLen > data->len) {
      data->left = msgLen - data->len;
      return false;
    } else if (msgLen == data->len) {
      return true;
    } else if (msgLen < data->len) {
      return false;
      // handle other packet later
    }
  } else {
    return false;
  }
}

// static void uvDoProcess(SRecvInfo* pRecv) {
//  // impl later
//  STransMsgHead* pHead = (STransMsgHead*)pRecv->msg;
//  SRpcInfo*      pRpc = (SRpcInfo*)pRecv->shandle;
//  SSrvConn*         pConn = pRecv->thandle;
//  tDump(pRecv->msg, pRecv->msgLen);
//  terrno = 0;
//  // SRpcReqContext* pContest;
//
//  // do auth and check
//}

static int uvAuthMsg(SSrvConn* pConn, char* msg, int len) {
  STransMsgHead* pHead = (STransMsgHead*)msg;

  int code = 0;

  if ((pConn->secured && pHead->spi == 0) || (pHead->spi == 0 && pConn->spi == 0)) {
    // secured link, or no authentication
    pHead->msgLen = (int32_t)htonl((uint32_t)pHead->msgLen);
    // tTrace("%s, secured link, no auth is required", pConn->info);
    return 0;
  }

  if (!rpcIsReq(pHead->msgType)) {
    // for response, if code is auth failure, it shall bypass the auth process
    code = htonl(pHead->code);
    if (code == TSDB_CODE_RPC_INVALID_TIME_STAMP || code == TSDB_CODE_RPC_AUTH_FAILURE ||
        code == TSDB_CODE_RPC_INVALID_VERSION || code == TSDB_CODE_RPC_AUTH_REQUIRED ||
        code == TSDB_CODE_MND_USER_NOT_EXIST || code == TSDB_CODE_RPC_NOT_READY) {
      pHead->msgLen = (int32_t)htonl((uint32_t)pHead->msgLen);
      // tTrace("%s, dont check authentication since code is:0x%x", pConn->info, code);
      return 0;
    }
  }

  code = 0;
  if (pHead->spi == pConn->spi) {
    // authentication
    SRpcDigest* pDigest = (SRpcDigest*)((char*)pHead + len - sizeof(SRpcDigest));

    int32_t delta;
    delta = (int32_t)htonl(pDigest->timeStamp);
    delta -= (int32_t)taosGetTimestampSec();
    if (abs(delta) > 900) {
      tWarn("%s, time diff:%d is too big, msg discarded", pConn->info, delta);
      code = TSDB_CODE_RPC_INVALID_TIME_STAMP;
    } else {
      if (rpcAuthenticateMsg(pHead, len - TSDB_AUTH_LEN, pDigest->auth, pConn->secret) < 0) {
        // tDebug("%s, authentication failed, msg discarded", pConn->info);
        code = TSDB_CODE_RPC_AUTH_FAILURE;
      } else {
        pHead->msgLen = (int32_t)htonl((uint32_t)pHead->msgLen) - sizeof(SRpcDigest);
        if (!rpcIsReq(pHead->msgType)) pConn->secured = 1;  // link is secured for client
        // tTrace("%s, message is authenticated", pConn->info);
      }
    }
  } else {
    tDebug("%s, auth spi:%d not matched with received:%d", pConn->info, pConn->spi, pHead->spi);
    code = pHead->spi ? TSDB_CODE_RPC_AUTH_FAILURE : TSDB_CODE_RPC_AUTH_REQUIRED;
  }

  return code;
}

// refers specifically to query or insert timeout
static void uvHandleActivityTimeout(uv_timer_t* handle) {
  SSrvConn* conn = handle->data;
  tDebug("%p timeout since no activity", conn);
}

static void uvHandleReq(SSrvConn* pConn) {
  SRecvInfo    info;
  SRecvInfo*   p = &info;
  SConnBuffer* pBuf = &pConn->readBuf;
  p->msg = pBuf->buf;
  p->msgLen = pBuf->len;
  p->ip = 0;
  p->port = 0;
  p->shandle = pConn->pTransInst;  //
  p->thandle = pConn;
  p->chandle = NULL;

  STransMsgHead* pHead = (STransMsgHead*)p->msg;
  if (pHead->secured == 1) {
    STransUserMsg* uMsg = (p->msg + p->msgLen - sizeof(STransUserMsg));
    memcpy(pConn->user, uMsg->user, tListLen(uMsg->user));
    memcpy(pConn->secret, uMsg->secret, tListLen(uMsg->secret));
  }

  pConn->inType = pHead->msgType;
  assert(transIsReq(pHead->msgType));

  SRpcInfo* pRpc = (SRpcInfo*)p->shandle;
  pHead->code = htonl(pHead->code);

  int32_t dlen = 0;
  if (transDecompressMsg(NULL, 0, NULL)) {
    // add compress later
    // pHead = rpcDecompressRpcMsg(pHead);
  } else {
    pHead->msgLen = htonl(pHead->msgLen);
    // impl later
    //
  }

  SRpcMsg rpcMsg;
  rpcMsg.contLen = transContLenFromMsg(pHead->msgLen);
  rpcMsg.pCont = pHead->content;
  rpcMsg.msgType = pHead->msgType;
  rpcMsg.code = pHead->code;
  rpcMsg.ahandle = NULL;
  rpcMsg.handle = pConn;

  transClearBuffer(&pConn->readBuf);
  pConn->ref++;
  tDebug("server conn %p %s received from %s:%d", pConn, TMSG_INFO(rpcMsg.msgType), inet_ntoa(pConn->addr.sin_addr),
         ntohs(pConn->addr.sin_port));
  (*(pRpc->cfp))(pRpc->parent, &rpcMsg, NULL);
  // uv_timer_start(pConn->pTimer, uvHandleActivityTimeout, pRpc->idleTime * 10000, 0);
  // auth
  // validate msg type
}

void uvOnReadCb(uv_stream_t* cli, ssize_t nread, const uv_buf_t* buf) {
  // opt
  SSrvConn*    conn = cli->data;
  SConnBuffer* pBuf = &conn->readBuf;
  if (nread > 0) {
    pBuf->len += nread;
    tTrace("server conn %p read summary, total read: %d, current read: %d", conn, pBuf->len, (int)nread);
    if (readComplete(pBuf)) {
      tTrace("server conn %p alread read complete packet", conn);
      uvHandleReq(conn);
    } else {
      tTrace("server %p read partial packet, continue to read", conn);
    }
    return;
  }
  if (nread == 0) {
    return;
  }
  if (nread < 0 || nread != UV_EOF) {
    if (conn->ref > 1) {
      conn->ref++;  // ref > 1 signed that write is in progress
    }
    tError("server conn %p read error: %s", conn, uv_err_name(nread));
    destroyConn(conn, true);
  }
}
void uvAllocConnBufferCb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  buf->base = malloc(sizeof(char));
  buf->len = 2;
}

void uvOnTimeoutCb(uv_timer_t* handle) {
  // opt
  SSrvConn* pConn = handle->data;
  tError("server conn %p time out", pConn);
}

void uvOnWriteCb(uv_write_t* req, int status) {
  SSrvConn* conn = req->data;

  SSrvMsg* smsg = conn->pSrvMsg;
  destroySmsg(smsg);
  conn->pSrvMsg = NULL;

  transClearBuffer(&conn->readBuf);
  if (status == 0) {
    tTrace("server conn %p data already was written on stream", conn);
  } else {
    tError("server conn %p failed to write data, %s", conn, uv_err_name(status));
    //
    destroyConn(conn, true);
  }
  // opt
}
static void uvOnPipeWriteCb(uv_write_t* req, int status) {
  if (status == 0) {
    tTrace("success to dispatch conn to work thread");
  } else {
    tError("fail to dispatch conn to work thread");
  }
}

static void uvPrepareSendData(SSrvMsg* smsg, uv_buf_t* wb) {
  // impl later;
  tTrace("server conn %p prepare to send resp", smsg->pConn);

  SSrvConn* pConn = smsg->pConn;
  SRpcMsg*  pMsg = &smsg->msg;
  if (pMsg->pCont == 0) {
    pMsg->pCont = (void*)rpcMallocCont(0);
    pMsg->contLen = 0;
  }
  STransMsgHead* pHead = transHeadFromCont(pMsg->pCont);

  pHead->secured = pMsg->code == 0 ? 1 : 0;  //
  pHead->msgType = smsg->pConn->inType + 1;
  pHead->code = htonl(pMsg->code);
  // add more info
  char*   msg = (char*)pHead;
  int32_t len = transMsgLenFromCont(pMsg->contLen);
  if (transCompressMsg(msg, len, NULL)) {
    // impl later
  }
  tDebug("server conn %p %s is sent to %s:%d", pConn, TMSG_INFO(pHead->msgType), inet_ntoa(pConn->addr.sin_addr),
         ntohs(pConn->addr.sin_port));

  pHead->msgLen = htonl(len);
  wb->base = msg;
  wb->len = len;
}
static void uvStartSendResp(SSrvMsg* smsg) {
  // impl
  uv_buf_t wb;
  uvPrepareSendData(smsg, &wb);

  SSrvConn* pConn = smsg->pConn;
  uv_timer_stop(pConn->pTimer);

  pConn->pSrvMsg = smsg;
  // conn->pWriter->data = smsg;
  uv_write(pConn->pWriter, (uv_stream_t*)pConn->pTcp, &wb, 1, uvOnWriteCb);

  // SRpcMsg* rpcMsg = smsg->msg;

  return;
}
static void destroySmsg(SSrvMsg* smsg) {
  if (smsg == NULL) {
    return;
  }
  transFreeMsg(smsg->msg.pCont);
  free(smsg);
}
void uvWorkerAsyncCb(uv_async_t* handle) {
  SAsyncItem*   item = handle->data;
  SWorkThrdObj* pThrd = item->pThrd;
  SSrvConn*     conn = NULL;
  queue         wq;
  // batch process to avoid to lock/unlock frequently
  pthread_mutex_lock(&item->mtx);
  QUEUE_MOVE(&item->qmsg, &wq);
  pthread_mutex_unlock(&item->mtx);
  // pthread_mutex_unlock(&mtx);

  while (!QUEUE_IS_EMPTY(&wq)) {
    queue* head = QUEUE_HEAD(&wq);
    QUEUE_REMOVE(head);

    SSrvMsg* msg = QUEUE_DATA(head, SSrvMsg, q);
    if (msg == NULL) {
      tError("unexcept occurred, continue");
      continue;
    }
    if (msg->pConn == NULL) {
      //
      free(msg);
      uv_stop(pThrd->loop);
    } else {
      uvStartSendResp(msg);
    }
    // uv_buf_t wb;
    // uvPrepareSendData(msg, &wb);
    // uv_timer_stop(conn->pTimer);

    // uv_write(conn->pWriter, (uv_stream_t*)conn->pTcp, &wb, 1, uvOnWriteCb);
  }
}
static void uvAcceptAsyncCb(uv_async_t* async) {
  SServerObj* srv = async->data;
  uv_stop(srv->loop);
}

void uvOnAcceptCb(uv_stream_t* stream, int status) {
  if (status == -1) {
    return;
  }
  SServerObj* pObj = container_of(stream, SServerObj, server);

  uv_tcp_t* cli = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
  uv_tcp_init(pObj->loop, cli);

  if (uv_accept(stream, (uv_stream_t*)cli) == 0) {
    uv_write_t* wr = (uv_write_t*)malloc(sizeof(uv_write_t));

    uv_buf_t buf = uv_buf_init((char*)notify, strlen(notify));

    pObj->workerIdx = (pObj->workerIdx + 1) % pObj->numOfThreads;

    tTrace("new conntion accepted by main server, dispatch to %dth worker-thread", pObj->workerIdx);
    uv_write2(wr, (uv_stream_t*)&(pObj->pipe[pObj->workerIdx][0]), &buf, 1, (uv_stream_t*)cli, uvOnPipeWriteCb);
  } else {
    uv_close((uv_handle_t*)cli, NULL);
    free(cli);
  }
}
void uvOnConnectionCb(uv_stream_t* q, ssize_t nread, const uv_buf_t* buf) {
  tTrace("server connection coming");
  if (nread < 0) {
    if (nread != UV_EOF) {
      tError("read error %s", uv_err_name(nread));
    }
    // TODO(log other failure reason)
    // uv_close((uv_handle_t*)q, NULL);
    return;
  }
  // free memory allocated by
  assert(nread == strlen(notify));
  assert(buf->base[0] == notify[0]);
  free(buf->base);

  SWorkThrdObj* pThrd = q->data;

  uv_pipe_t* pipe = (uv_pipe_t*)q;
  if (!uv_pipe_pending_count(pipe)) {
    tError("No pending count");
    return;
  }

  uv_handle_type pending = uv_pipe_pending_type(pipe);
  assert(pending == UV_TCP);

  SSrvConn* pConn = createConn();

  pConn->pTransInst = pThrd->pTransInst;
  /* init conn timer*/
  pConn->pTimer = malloc(sizeof(uv_timer_t));
  uv_timer_init(pThrd->loop, pConn->pTimer);
  pConn->pTimer->data = pConn;

  pConn->hostThrd = pThrd;
  // pConn->pWorkerAsync = pThrd->workerAsync;  // thread safty

  // init client handle
  pConn->pTcp = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
  uv_tcp_init(pThrd->loop, pConn->pTcp);
  pConn->pTcp->data = pConn;

  // init write request, just
  pConn->pWriter = calloc(1, sizeof(uv_write_t));
  pConn->pWriter->data = pConn;

  if (uv_accept(q, (uv_stream_t*)(pConn->pTcp)) == 0) {
    uv_os_fd_t fd;
    uv_fileno((const uv_handle_t*)pConn->pTcp, &fd);
    tTrace("server conn %p created, fd: %d", pConn, fd);
    int addrlen = sizeof(pConn->addr);
    if (0 != uv_tcp_getpeername(pConn->pTcp, (struct sockaddr*)&pConn->addr, &addrlen)) {
      tError("server conn %p failed to get peer info", pConn);
      destroyConn(pConn, true);
    } else {
      uv_read_start((uv_stream_t*)(pConn->pTcp), uvAllocReadBufferCb, uvOnReadCb);
    }
  } else {
    tDebug("failed to create new connection");
    destroyConn(pConn, true);
  }
}

void* acceptThread(void* arg) {
  // opt
  setThreadName("trans-accept");
  SServerObj* srv = (SServerObj*)arg;
  uv_run(srv->loop, UV_RUN_DEFAULT);
}
static bool addHandleToWorkloop(void* arg) {
  SWorkThrdObj* pThrd = arg;
  pThrd->loop = (uv_loop_t*)malloc(sizeof(uv_loop_t));
  if (0 != uv_loop_init(pThrd->loop)) {
    return false;
  }

  // SRpcInfo* pRpc = pThrd->shandle;
  uv_pipe_init(pThrd->loop, pThrd->pipe, 1);
  uv_pipe_open(pThrd->pipe, pThrd->fd);

  pThrd->pipe->data = pThrd;

  QUEUE_INIT(&pThrd->msg);
  pthread_mutex_init(&pThrd->msgMtx, NULL);

  pThrd->asyncPool = transCreateAsyncPool(pThrd->loop, pThrd, uvWorkerAsyncCb);
  uv_read_start((uv_stream_t*)pThrd->pipe, uvAllocConnBufferCb, uvOnConnectionCb);
  return true;
}

static bool addHandleToAcceptloop(void* arg) {
  // impl later
  SServerObj* srv = arg;

  int err = 0;
  if ((err = uv_tcp_init(srv->loop, &srv->server)) != 0) {
    tError("failed to init accept server: %s", uv_err_name(err));
    return false;
  }

  // register an async here to quit server gracefully
  srv->pAcceptAsync = calloc(1, sizeof(uv_async_t));
  uv_async_init(srv->loop, srv->pAcceptAsync, uvAcceptAsyncCb);
  srv->pAcceptAsync->data = srv;

  struct sockaddr_in bind_addr;
  uv_ip4_addr("0.0.0.0", srv->port, &bind_addr);
  if ((err = uv_tcp_bind(&srv->server, (const struct sockaddr*)&bind_addr, 0)) != 0) {
    tError("failed to bind: %s", uv_err_name(err));
    return false;
  }
  if ((err = uv_listen((uv_stream_t*)&srv->server, 512, uvOnAcceptCb)) != 0) {
    tError("failed to listen: %s", uv_err_name(err));
    return false;
  }
  return true;
}
void* workerThread(void* arg) {
  setThreadName("trans-worker");
  SWorkThrdObj* pThrd = (SWorkThrdObj*)arg;
  uv_run(pThrd->loop, UV_RUN_DEFAULT);
}

static SSrvConn* createConn() {
  SSrvConn* pConn = (SSrvConn*)calloc(1, sizeof(SSrvConn));
  tTrace("conn %p created", pConn);
  ++pConn->ref;
  return pConn;
}

static void destroyConn(SSrvConn* conn, bool clear) {
  if (conn == NULL) {
    return;
  }
  tTrace("server conn %p try to destroy", conn);
  if (--conn->ref > 0) {
    return;
  }
  transDestroyBuffer(&conn->readBuf);
  destroySmsg(conn->pSrvMsg);
  conn->pSrvMsg = NULL;

  if (clear) {
    uv_close((uv_handle_t*)conn->pTcp, uvDestroyConn);
  }
}
static void uvDestroyConn(uv_handle_t* handle) {
  SSrvConn* conn = handle->data;
  tDebug("server conn %p destroy", conn);
  uv_timer_stop(conn->pTimer);
  free(conn->pTimer);
  // free(conn->pTcp);
  free(conn->pWriter);
  free(conn);
}
static int transAddAuthPart(SSrvConn* pConn, char* msg, int msgLen) {
  STransMsgHead* pHead = (STransMsgHead*)msg;

  if (pConn->spi && pConn->secured == 0) {
    // add auth part
    pHead->spi = pConn->spi;
    STransDigestMsg* pDigest = (STransDigestMsg*)(msg + msgLen);
    pDigest->timeStamp = htonl(taosGetTimestampSec());
    msgLen += sizeof(SRpcDigest);
    pHead->msgLen = (int32_t)htonl((uint32_t)msgLen);
    // transBuildAuthHead(pHead, msgLen - TSDB_AUTH_LEN, pDigest->auth, pConn->secret);
    // transBuildAuthHead(pHead, msgLen - TSDB_AUTH_LEN, pDigest->auth, pConn->secret);
  } else {
    pHead->spi = 0;
    pHead->msgLen = (int32_t)htonl((uint32_t)msgLen);
  }

  return msgLen;
}

void* taosInitServer(uint32_t ip, uint32_t port, char* label, int numOfThreads, void* fp, void* shandle) {
  SServerObj* srv = calloc(1, sizeof(SServerObj));
  srv->loop = (uv_loop_t*)malloc(sizeof(uv_loop_t));
  srv->numOfThreads = numOfThreads;
  srv->workerIdx = 0;
  srv->pThreadObj = (SWorkThrdObj**)calloc(srv->numOfThreads, sizeof(SWorkThrdObj*));
  srv->pipe = (uv_pipe_t**)calloc(srv->numOfThreads, sizeof(uv_pipe_t*));
  srv->ip = ip;
  srv->port = port;
  uv_loop_init(srv->loop);

  for (int i = 0; i < srv->numOfThreads; i++) {
    SWorkThrdObj* thrd = (SWorkThrdObj*)calloc(1, sizeof(SWorkThrdObj));
    srv->pThreadObj[i] = thrd;

    srv->pipe[i] = (uv_pipe_t*)calloc(2, sizeof(uv_pipe_t));
    int fds[2];
    if (uv_socketpair(AF_UNIX, SOCK_STREAM, fds, UV_NONBLOCK_PIPE, UV_NONBLOCK_PIPE) != 0) {
      goto End;
    }
    uv_pipe_init(srv->loop, &(srv->pipe[i][0]), 1);
    uv_pipe_open(&(srv->pipe[i][0]), fds[1]);  // init write

    thrd->pTransInst = shandle;
    thrd->fd = fds[0];
    thrd->pipe = &(srv->pipe[i][1]);  // init read

    if (false == addHandleToWorkloop(thrd)) {
      goto End;
    }
    int err = pthread_create(&(thrd->thread), NULL, workerThread, (void*)(thrd));
    if (err == 0) {
      tDebug("sucess to create worker-thread %d", i);
      // printf("thread %d create\n", i);
    } else {
      // TODO: clear all other resource later
      tError("failed to create worker-thread %d", i);
    }
  }
  if (false == addHandleToAcceptloop(srv)) {
    goto End;
  }
  int err = pthread_create(&srv->thread, NULL, acceptThread, (void*)srv);
  if (err == 0) {
    tDebug("success to create accept-thread");
  } else {
    // clear all resource later
  }

  return srv;
End:
  taosCloseServer(srv);
  return NULL;
}

void destroyWorkThrd(SWorkThrdObj* pThrd) {
  if (pThrd == NULL) {
    return;
  }
  pthread_join(pThrd->thread, NULL);
  free(pThrd->loop);
  transDestroyAsyncPool(pThrd->asyncPool);

  // free(pThrd->workerAsync);
  free(pThrd);
}
void sendQuitToWorkThrd(SWorkThrdObj* pThrd) {
  SSrvMsg* srvMsg = calloc(1, sizeof(SSrvMsg));

  // pthread_mutex_lock(&pThrd->msgMtx);
  // QUEUE_PUSH(&pThrd->msg, &srvMsg->q);
  // pthread_mutex_unlock(&pThrd->msgMtx);
  tDebug("send quit msg to work thread");

  transSendAsync(pThrd->asyncPool, &srvMsg->q);
  // uv_async_send(pThrd->workerAsync);
}

void taosCloseServer(void* arg) {
  // impl later
  SServerObj* srv = arg;
  for (int i = 0; i < srv->numOfThreads; i++) {
    sendQuitToWorkThrd(srv->pThreadObj[i]);
    destroyWorkThrd(srv->pThreadObj[i]);
  }

  tDebug("send quit msg to accept thread");
  uv_async_send(srv->pAcceptAsync);
  pthread_join(srv->thread, NULL);

  free(srv->pThreadObj);
  free(srv->pAcceptAsync);
  free(srv->loop);

  for (int i = 0; i < srv->numOfThreads; i++) {
    free(srv->pipe[i]);
  }
  free(srv->pipe);

  free(srv);
}

void rpcSendResponse(const SRpcMsg* pMsg) {
  if (pMsg->handle == NULL) {
    return;
  }
  SSrvConn*     pConn = pMsg->handle;
  SWorkThrdObj* pThrd = pConn->hostThrd;

  SSrvMsg* srvMsg = calloc(1, sizeof(SSrvMsg));
  srvMsg->pConn = pConn;
  srvMsg->msg = *pMsg;

  // pthread_mutex_lock(&pThrd->msgMtx);
  // QUEUE_PUSH(&pThrd->msg, &srvMsg->q);
  // pthread_mutex_unlock(&pThrd->msgMtx);

  tTrace("server conn %p start to send resp", pConn);
  transSendAsync(pThrd->asyncPool, &srvMsg->q);
  // uv_async_send(pThrd->workerAsync);
}

int rpcGetConnInfo(void* thandle, SRpcConnInfo* pInfo) {
  SSrvConn* pConn = thandle;
  // struct sockaddr* pPeerName = &pConn->peername;

  struct sockaddr_in addr = pConn->addr;
  pInfo->clientIp = (uint32_t)(addr.sin_addr.s_addr);
  pInfo->clientPort = ntohs(addr.sin_port);

  tstrncpy(pInfo->user, pConn->user, sizeof(pInfo->user));
  return 0;
}

#endif