/*
 * Filename: /mnt/spark-pmof/tool/rpmp/pmpool/client/NetworkClient.cc
 * Path: /mnt/spark-pmof/tool/rpmp/pmpool/client
 * Created Date: Monday, December 16th 2019, 1:16:16 pm
 * Author: root
 *
 * Copyright (c) 2019 Intel
 */

#include "pmpool/client/NetworkClient.h"

#include <HPNL/Callback.h>
#include <HPNL/ChunkMgr.h>
#include <HPNL/Connection.h>

#include "pmpool/Event.h"
#include "pmpool/buffer/CircularBuffer.h"
using namespace std::chrono_literals;

uint64_t timestamp_now() {
  return std::chrono::high_resolution_clock::now().time_since_epoch() /
         std::chrono::milliseconds(1);
}

RequestHandler::RequestHandler(std::shared_ptr<NetworkClient> networkClient)
    : networkClient_(networkClient) {}

void RequestHandler::addTask(std::shared_ptr<Request> request) {
  pendingRequestQueue_.enqueue(request);
}

void RequestHandler::addTask(std::shared_ptr<Request> request,
                             std::function<void()> func) {
  callback_map[request->get_rc().rid] = func;
  pendingRequestQueue_.enqueue(request);
}

int RequestHandler::entry() {
  std::shared_ptr<Request> request;
  bool res = pendingRequestQueue_.wait_dequeue_timed(
      request, std::chrono::milliseconds(1000));
  if (res) {
    handleRequest(request);
  }
  return 0;
}

std::shared_ptr<RequestHandler::InflightRequestContext>
RequestHandler::inflight_insert_or_get(std::shared_ptr<Request> request) {
  const std::lock_guard<std::mutex> lock(inflight_mtx_);
  if (inflight_.find(request) == inflight_.end()) {
    auto ctx = std::make_shared<InflightRequestContext>();
    inflight_.emplace(request, ctx);
    return ctx;
  } else {
    auto ctx = inflight_[request];
    return ctx;
  }
}

void RequestHandler::inflight_erase(std::shared_ptr<Request> request) {
  const std::lock_guard<std::mutex> lock(inflight_mtx_);
  inflight_.erase(request);
}

std::shared_ptr<RequestReplyContext> RequestHandler::get(
    std::shared_ptr<Request> request) {
  auto ctx = inflight_insert_or_get(request);
  unique_lock<mutex> lk(ctx->mtx_reply);
  while (!ctx->cv_reply.wait_for(lk, 5ms, [ctx] { return ctx->op_finished; })) {
  }
  auto res = requestReplyContext;
  ctx->op_returned = true;
  ctx->cv_returned.notify_one();
  return res;
}

void RequestHandler::notify(std::shared_ptr<RequestReply> requestReply) {
  const std::lock_guard<std::mutex> lock(inflight_mtx_);
  auto ctx = inflight_[currentRequest];
  ctx->op_finished = true;
  auto rrc = requestReply->get_rrc();
  if (expectedReturnType != rrc->type) {
    std::string err_msg = "expected return type is " +
                          std::to_string(expectedReturnType) +
                          ", current rrc.type is " + std::to_string(rrc->type);
    std::cout << err_msg << std::endl;
    return;
  }
  requestReplyContext = rrc;
  if (callback_map.count(requestReplyContext->rid) != 0) {
    callback_map[requestReplyContext->rid]();
    callback_map.erase(requestReplyContext->rid);
  } else {
    ctx->cv_reply.notify_one();
  }
}

void RequestHandler::handleRequest(std::shared_ptr<Request> request) {
  auto ctx = inflight_insert_or_get(request);
  currentRequest = request;
  OpType rt = request->get_rc().type;
  switch (rt) {
    case ALLOC: {
      expectedReturnType = ALLOC_REPLY;
      request->encode();
      networkClient_->send(reinterpret_cast<char *>(request->data_),
                           request->size_);
      break;
    }
    case FREE: {
      expectedReturnType = FREE_REPLY;
      request->encode();
      networkClient_->send(reinterpret_cast<char *>(request->data_),
                           request->size_);
      break;
    }
    case WRITE: {
      expectedReturnType = WRITE_REPLY;
      request->encode();
      networkClient_->send(reinterpret_cast<char *>(request->data_),
                           request->size_);
      break;
    }
    case READ: {
      expectedReturnType = READ_REPLY;
      request->encode();
      networkClient_->send(reinterpret_cast<char *>(request->data_),
                           request->size_);
      break;
    }
    case PUT: {
      expectedReturnType = PUT_REPLY;
      request->encode();
      networkClient_->send(reinterpret_cast<char *>(request->data_),
                           request->size_);
      break;
    }
    case GET_META: {
      expectedReturnType = GET_META_REPLY;
      request->encode();
      networkClient_->send(reinterpret_cast<char *>(request->data_),
                           request->size_);
      break;
    }
    default: {}
  }
  unique_lock<mutex> lk(ctx->mtx_returned);
  while (
      !ctx->cv_returned.wait_for(lk, 5ms, [ctx] { return ctx->op_returned; })) {
  }
  inflight_erase(request);
}

ClientConnectedCallback::ClientConnectedCallback(
    std::shared_ptr<NetworkClient> networkClient) {
  networkClient_ = networkClient;
}

void ClientConnectedCallback::operator()(void *param_1, void *param_2) {
  auto con = static_cast<Connection *>(param_1);
  networkClient_->connected(con);
}

ClientRecvCallback::ClientRecvCallback(
    std::shared_ptr<ChunkMgr> chunkMgr,
    std::shared_ptr<RequestHandler> requestHandler)
    : chunkMgr_(chunkMgr), requestHandler_(requestHandler) {}

void ClientRecvCallback::operator()(void *param_1, void *param_2) {
  int mid = *static_cast<int *>(param_1);
  auto ck = chunkMgr_->get(mid);

  // test start
  // auto con = reinterpret_cast<Connection*>(ck->con);
  // if (count_ == 0) {
  //   start = timestamp_now();
  // }
  // count_++;
  // if (count_ >= 1000000) {
  //   end = timestamp_now();
  //   std::cout << "consumes " << (end-start)/1000.0 << std::endl;
  //   return;
  // }
  // RequestContext rc = {};
  // rc.type = READ;
  // rc.rid = 0;
  // rc.size = 0;
  // rc.address = 0;
  // Request request(rc);
  // request.encode();
  // auto new_ck = chunkMgr_->get(con);
  // memcpy(new_ck->buffer, request.data_, request.size_);
  // new_ck->size = request.size_;
  // con->send(new_ck);
  // test end

  auto requestReply = std::make_shared<RequestReply>(
      reinterpret_cast<char *>(ck->buffer), ck->size,
      reinterpret_cast<Connection *>(ck->con));
  requestReply->decode();
  auto rrc = requestReply->get_rrc();
  switch (rrc->type) {
    case ALLOC_REPLY: {
      requestHandler_->notify(requestReply);
      break;
    }
    case FREE_REPLY: {
      requestHandler_->notify(requestReply);
      break;
    }
    case WRITE_REPLY: {
      requestHandler_->notify(requestReply);
      break;
    }
    case READ_REPLY: {
      requestHandler_->notify(requestReply);
      break;
    }
    case PUT_REPLY: {
      requestHandler_->notify(requestReply);
      break;
    }
    case GET_META_REPLY: {
      requestHandler_->notify(requestReply);
      break;
    }
    default: {}
  }
  chunkMgr_->reclaim(ck, static_cast<Connection *>(ck->con));
}

NetworkClient::NetworkClient(const string &remote_address,
                             const string &remote_port)
    : NetworkClient(remote_address, remote_port, 1, 32, 65536, 64) {}

NetworkClient::NetworkClient(const string &remote_address,
                             const string &remote_port, int worker_num,
                             int buffer_num_per_con, int buffer_size,
                             int init_buffer_num)
    : remote_address_(remote_address),
      remote_port_(remote_port),
      worker_num_(worker_num),
      buffer_num_per_con_(buffer_num_per_con),
      buffer_size_(buffer_size),
      init_buffer_num_(init_buffer_num),
      connected_(false) {}

NetworkClient::~NetworkClient() {}

int NetworkClient::init(std::shared_ptr<RequestHandler> requestHandler) {
  client_ = new Client(worker_num_, buffer_num_per_con_);
  if ((client_->init()) != 0) {
    return -1;
  }
  chunkMgr_ =
      std::make_shared<ChunkPool>(client_, buffer_size_, init_buffer_num_);

  client_->set_chunk_mgr(chunkMgr_.get());

  shutdownCallback = std::make_shared<ClientShutdownCallback>();
  connectedCallback =
      std::make_shared<ClientConnectedCallback>(shared_from_this());
  recvCallback =
      std::make_shared<ClientRecvCallback>(chunkMgr_, requestHandler);
  sendCallback = std::make_shared<ClientSendCallback>(chunkMgr_);

  client_->set_shutdown_callback(shutdownCallback.get());
  client_->set_connected_callback(connectedCallback.get());
  client_->set_recv_callback(recvCallback.get());
  client_->set_send_callback(sendCallback.get());

  client_->start();
  int res = client_->connect(remote_address_.c_str(), remote_port_.c_str());
  unique_lock<mutex> lk(con_mtx);
  while (!connected_) {
    con_v.wait(lk);
  }

  circularBuffer_ =
      make_shared<CircularBuffer>(1024 * 1024, 512, false, shared_from_this());
  return 0;
}

void NetworkClient::shutdown() { client_->shutdown(); }

void NetworkClient::wait() { client_->wait(); }

Chunk *NetworkClient::register_rma_buffer(char *rma_buffer, uint64_t size) {
  return client_->reg_rma_buffer(rma_buffer, size, buffer_id_++);
}

void NetworkClient::unregister_rma_buffer(int buffer_id) {
  client_->unreg_rma_buffer(buffer_id);
}

uint64_t NetworkClient::get_dram_buffer(const char *data, uint64_t size) {
  char *dest = circularBuffer_->get(size);
  if (data) {
    memcpy(dest, data, size);
  }
  return (uint64_t)dest;
}

void NetworkClient::reclaim_dram_buffer(uint64_t src_address, uint64_t size) {
  circularBuffer_->put(reinterpret_cast<char *>(src_address), size);
}

uint64_t NetworkClient::get_rkey() {
  return circularBuffer_->get_rma_chunk()->mr->key;
}

void NetworkClient::connected(Connection *con) {
  std::unique_lock<std::mutex> lk(con_mtx);
  con_ = con;
  connected_ = true;
  con_v.notify_all();
  lk.unlock();
}

void NetworkClient::send(char *data, uint64_t size) {
  auto ck = chunkMgr_->get(con_);
  std::memcpy(reinterpret_cast<char *>(ck->buffer), data, size);
  ck->size = size;
#ifdef DEBUG
  RequestMsg *requestMsg = (RequestMsg *)(data);
  std::cout << "[NetworkClient::send][" << requestMsg->type << "] size is "
            << size << std::endl;
  for (int i = 0; i < size; i++) {
    printf("%X ", *(data + i));
  }
  printf("\n");
#endif
  con_->send(ck);
}

void NetworkClient::read(std::shared_ptr<Request> request) {
  RequestContext rc = request->get_rc();
}