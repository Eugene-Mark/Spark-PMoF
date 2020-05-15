/*
 * Filename: /mnt/spark-pmof/tool/rpmp/pmpool/Protocol.cc
 * Path: /mnt/spark-pmof/tool/rpmp/pmpool
 * Created Date: Thursday, November 7th 2019, 3:48:52 pm
 * Author: root
 *
 * Copyright (c) 2019 Intel
 */

#include "pmpool/Protocol.h"

#include <assert.h>

#include "pmpool/AllocatorProxy.h"
#include "pmpool/Config.h"
#include "pmpool/Digest.h"
#include "pmpool/Event.h"
#include "pmpool/Log.h"
#include "pmpool/NetworkServer.h"

RecvCallback::RecvCallback(std::shared_ptr<Protocol> protocol,
                           std::shared_ptr<ChunkMgr> chunkMgr)
    : protocol_(protocol), chunkMgr_(chunkMgr) {}

void RecvCallback::operator()(void *buffer_id, void *buffer_size) {
  auto buffer_id_ = *static_cast<int *>(buffer_id);
  Chunk *ck = chunkMgr_->get(buffer_id_);
  assert(*static_cast<uint64_t *>(buffer_size) == ck->size);
  auto request =
      std::make_shared<Request>(reinterpret_cast<char *>(ck->buffer), ck->size,
                                reinterpret_cast<Connection *>(ck->con));
  request->decode();
  RequestMsg *requestMsg = (RequestMsg *)(request->getData());
  if (requestMsg->type != 0) {
    protocol_->enqueue_recv_msg(request);
  } else {
    std::cout << "[RecvCallback::RecvCallback][" << requestMsg->type
              << "] size is " << ck->size << std::endl;
    for (int i = 0; i < ck->size; i++) {
      printf("%X ", *(request->getData() + i));
    }
    printf("\n");
  }
  chunkMgr_->reclaim(ck, static_cast<Connection *>(ck->con));
}

ReadCallback::ReadCallback(std::shared_ptr<Protocol> protocol)
    : protocol_(protocol) {}

void ReadCallback::operator()(void *buffer_id, void *buffer_size) {
  auto buffer_id_ = *static_cast<int *>(buffer_id);
  protocol_->enqueue_rma_msg(buffer_id_);
}

SendCallback::SendCallback(
    std::shared_ptr<ChunkMgr> chunkMgr,
    std::unordered_map<uint64_t, std::shared_ptr<RequestReply>> rrcMap)
    : chunkMgr_(chunkMgr), rrcMap_(rrcMap) {}

void SendCallback::operator()(void *buffer_id, void *buffer_size) {
  auto buffer_id_ = *static_cast<int *>(buffer_id);
  auto ck = chunkMgr_->get(buffer_id_);

  /// free the memory of class RequestReply
  rrcMap_.erase(buffer_id_);

  chunkMgr_->reclaim(ck, static_cast<Connection *>(ck->con));
}

WriteCallback::WriteCallback(std::shared_ptr<Protocol> protocol)
    : protocol_(protocol) {}

void WriteCallback::operator()(void *buffer_id, void *buffer_size) {
  auto buffer_id_ = *static_cast<int *>(buffer_id);
  protocol_->enqueue_rma_msg(buffer_id_);
}

RecvWorker::RecvWorker(std::shared_ptr<Protocol> protocol, int index)
    : protocol_(protocol), index_(index) {
  init = false;
}

int RecvWorker::entry() {
  if (!init) {
    // set_affinity(index_);
    init = true;
  }
  std::shared_ptr<Request> request;
  bool res = pendingRecvRequestQueue_.wait_dequeue_timed(
      request, std::chrono::milliseconds(1000));
  if (res) {
    protocol_->handle_recv_msg(request);
  }
  return 0;
}

void RecvWorker::abort() {}

void RecvWorker::addTask(std::shared_ptr<Request> request) {
  pendingRecvRequestQueue_.enqueue(request);
}

ReadWorker::ReadWorker(std::shared_ptr<Protocol> protocol, int index)
    : protocol_(protocol), index_(index) {
  init = false;
}

int ReadWorker::entry() {
  if (!init) {
    // set_affinity(index_);
    init = true;
  }
  std::shared_ptr<RequestReply> requestReply;
  bool res = pendingReadRequestQueue_.wait_dequeue_timed(
      requestReply, std::chrono::milliseconds(1000));
  if (res) {
    protocol_->handle_rma_msg(requestReply);
  }
  return 0;
}

void ReadWorker::abort() {}

void ReadWorker::addTask(std::shared_ptr<RequestReply> rr) {
  pendingReadRequestQueue_.enqueue(rr);
}

FinalizeWorker::FinalizeWorker(std::shared_ptr<Protocol> protocol)
    : protocol_(protocol) {}

int FinalizeWorker::entry() {
  std::shared_ptr<RequestReply> requestReply;
  bool res = pendingRequestReplyQueue_.wait_dequeue_timed(
      requestReply, std::chrono::milliseconds(1000));
  assert(res);
  if (res) {
    protocol_->handle_finalize_msg(requestReply);
  }
  return 0;
}

void FinalizeWorker::abort() {}

void FinalizeWorker::addTask(std::shared_ptr<RequestReply> requestReply) {
  pendingRequestReplyQueue_.enqueue(requestReply);
}

Protocol::Protocol(std::shared_ptr<Config> config, std::shared_ptr<Log> log,
                   std::shared_ptr<NetworkServer> server,
                   std::shared_ptr<AllocatorProxy> allocatorProxy)
    : config_(config),
      log_(log),
      networkServer_(server),
      allocatorProxy_(allocatorProxy) {
  time = 0;
  char *buffer = new char[2147483648]();
  memset(buffer, 0, 2147483648);
  address = allocatorProxy_->allocate_and_write(2147483648, buffer, 0);
  delete[] buffer;
}

Protocol::~Protocol() {
  for (auto worker : recvWorkers_) {
    worker->stop();
    worker->join();
  }
  for (auto worker : readWorkers_) {
    worker->stop();
    worker->join();
  }
  finalizeWorker_->stop();
  finalizeWorker_->join();
}

int Protocol::init() {
  recvCallback_ = std::make_shared<RecvCallback>(
      shared_from_this(), networkServer_->get_chunk_mgr());
  sendCallback_ =
      std::make_shared<SendCallback>(networkServer_->get_chunk_mgr(), rrcMap_);
  readCallback_ = std::make_shared<ReadCallback>(shared_from_this());
  writeCallback_ = std::make_shared<WriteCallback>(shared_from_this());

  for (int i = 0; i < config_->get_pool_size(); i++) {
    auto recvWorker = std::make_shared<RecvWorker>(
        shared_from_this(), config_->get_affinities_()[i] - 1);
    recvWorker->start();
    recvWorkers_.push_back(std::move(recvWorker));
  }

  finalizeWorker_ = make_shared<FinalizeWorker>(shared_from_this());
  finalizeWorker_->start();

  for (int i = 0; i < config_->get_pool_size(); i++) {
    auto readWorker = std::make_shared<ReadWorker>(
        shared_from_this(), config_->get_affinities_()[i]);
    readWorker->start();
    readWorkers_.push_back(std::move(readWorker));
  }

  networkServer_->set_recv_callback(recvCallback_.get());
  networkServer_->set_send_callback(sendCallback_.get());
  networkServer_->set_read_callback(readCallback_.get());
  networkServer_->set_write_callback(writeCallback_.get());
  return 0;
}

void Protocol::enqueue_recv_msg(std::shared_ptr<Request> request) {
  RequestContext rc = request->get_rc();
  if (rc.address != 0) {
    auto wid = GET_WID(rc.address);
    recvWorkers_[wid]->addTask(request);
  } else {
    recvWorkers_[rc.rid % config_->get_pool_size()]->addTask(request);
  }
}

void Protocol::handle_recv_msg(std::shared_ptr<Request> request) {
  num_requests_++;
  if (num_requests_ % 10000 == 0) {
    log_->get_file_log()->info(
        "Protocol::handle_recv_msg handled requests number is {0}.",
        num_requests_);
    log_->get_console_log()->info(
        "Protocol::handle_recv_msg handled requests number is {0}.",
        num_requests_);
  }
  RequestContext rc = request->get_rc();
  auto rrc = std::make_shared<RequestReplyContext>();
  switch (rc.type) {
    case ALLOC: {
      log_->get_console_log()->info(
          "Protocol::handle_recv_msg Allocate request.");
      uint64_t addr = allocatorProxy_->allocate_and_write(
          rc.size, nullptr, rc.rid % config_->get_pool_size());
      auto wid = GET_WID(addr);
      assert(wid == rc.rid % config_->get_pool_size());
      rrc->type = ALLOC_REPLY;
      rrc->success = 0;
      rrc->rid = rc.rid;
      rrc->address = addr;
      rrc->size = rc.size;
      rrc->con = rc.con;
      networkServer_->get_dram_buffer(rrc);
      std::shared_ptr<RequestReply> requestReply =
          std::make_shared<RequestReply>(rrc);
      rrc->ck->ptr = requestReply.get();
      enqueue_finalize_msg(requestReply);
      break;
    }
    case FREE: {
      log_->get_console_log()->info("Protocol::handle_recv_msg Free request.");
      rrc->type = FREE_REPLY;
      rrc->success = allocatorProxy_->release(rc.address);
      rrc->rid = rc.rid;
      rrc->address = rc.address;
      rrc->size = rc.size;
      rrc->con = rc.con;
      std::shared_ptr<RequestReply> requestReply =
          std::make_shared<RequestReply>(rrc);
      rrc->ck->ptr = requestReply.get();
      enqueue_finalize_msg(requestReply);
      break;
    }
    case WRITE: {
      log_->get_console_log()->info("Protocol::handle_recv_msg Write request.");
      rrc->type = WRITE_REPLY;
      rrc->success = 0;
      rrc->rid = rc.rid;
      rrc->address = rc.address;
      rrc->src_address = rc.src_address;
      rrc->src_rkey = rc.src_rkey;
      rrc->size = rc.size;
      rrc->con = rc.con;
      networkServer_->get_dram_buffer(rrc);
      std::shared_ptr<RequestReply> requestReply =
          std::make_shared<RequestReply>(rrc);
      rrc->ck->ptr = requestReply.get();

      std::unique_lock<std::mutex> lk(rrcMtx_);
      rrcMap_[rrc->ck->buffer_id] = requestReply;
      lk.unlock();
      networkServer_->read(requestReply);
      break;
    }
    case READ: {
#ifdef DEBUG
      std::cout << "[Protocol::handle_recv_msg][READ], info is " << rc.address
                << "-" << rc.size << std::endl;
#endif
      rrc->type = READ_REPLY;
      rrc->success = 0;
      rrc->rid = rc.rid;
      rrc->address = rc.address;
      rrc->src_address = rc.src_address;
      rrc->src_rkey = rc.src_rkey;
      rrc->size = rc.size;
      rrc->con = rc.con;
      rrc->dest_address = allocatorProxy_->get_virtual_address(rrc->address);
      rrc->ck = nullptr;
      Chunk *base_ck = allocatorProxy_->get_rma_chunk(rrc->address);
      networkServer_->get_pmem_buffer(rrc, base_ck);
      std::shared_ptr<RequestReply> requestReply =
          std::make_shared<RequestReply>(rrc);
      rrc->ck->ptr = requestReply.get();

      std::unique_lock<std::mutex> lk(rrcMtx_);
      rrcMap_[rrc->ck->buffer_id] = requestReply;
      lk.unlock();
      networkServer_->write(requestReply);
      break;
    }
    case PUT: {
      // log_->get_console_log()->info("Protocol::handle_recv_msg put
      // request.");
      rrc->type = PUT_REPLY;
      rrc->success = 0;
      rrc->rid = rc.rid;
      rrc->address = rc.address;
      rrc->src_address = rc.src_address;
      rrc->src_rkey = rc.src_rkey;
      rrc->size = rc.size;
      rrc->key = rc.key;
      rrc->con = rc.con;
      networkServer_->get_dram_buffer(rrc);
      std::shared_ptr<RequestReply> requestReply =
          std::make_shared<RequestReply>(rrc);
      /// bypass real workload to rpmp pool ///
      /*requestReply->encode();
      networkServer_->send(reinterpret_cast<char *>(requestReply->data_),
                           requestReply->size_, rrc->con);*/
      ////////////////////////////////////////////
      rrc->ck->ptr = requestReply.get();  // rrc->ck set by get_dram_buffer

      std::unique_lock<std::mutex> lk(rrcMtx_);
      rrcMap_[rrc->ck->buffer_id] = requestReply;
      lk.unlock();
      networkServer_->read(requestReply);
      break;
    }
    case GET_META: {
      // log_->get_console_log()->info(
      //    "Protocol::handle_recv_msg getMeta request.");
      rrc->type = GET_META_REPLY;
      rrc->success = 0;
      rrc->rid = rc.rid;
      rrc->size = rc.size;
      rrc->key = rc.key;
      rrc->con = rc.con;
      std::shared_ptr<RequestReply> requestReply =
          std::make_shared<RequestReply>(rrc);
      // rrc->ck->ptr = requestReply.get();
      /// bypass real workload to rpmp pool ///
      /*vector<block_meta> bml;
      bml.push_back(block_meta(address, rc.size, networkServer_->get_rkey()));
      requestReply->requestReplyContext_->bml = bml;
      requestReply->encode();
      networkServer_->send(reinterpret_cast<char *>(requestReply->data_),
                           requestReply->size_, rrc->con);*/
      //////////////////////////////////////////
      enqueue_finalize_msg(requestReply);
      break;
    }
    case DELETE: {
      log_->get_console_log()->info(
          "Protocol::handle_recv_msg Delete request.");
      rrc->type = DELETE_REPLY;
      rrc->key = rc.key;
      rrc->con = rc.con;
      rrc->rid = rc.rid;
      rrc->success = 0;
    }
    default: { break; }
  }
}

void Protocol::enqueue_finalize_msg(
    std::shared_ptr<RequestReply> requestReply) {
  finalizeWorker_->addTask(requestReply);
}

void Protocol::handle_finalize_msg(std::shared_ptr<RequestReply> requestReply) {
  std::shared_ptr<RequestReplyContext> rrc = requestReply->get_rrc();
  if (rrc->type == PUT_REPLY) {
    allocatorProxy_->cache_chunk(rrc->key, rrc->address, rrc->size,
                                 networkServer_->get_rkey());
  } else if (rrc->type == GET_META_REPLY) {
    auto bml = allocatorProxy_->get_cached_chunk(rrc->key);
    requestReply->requestReplyContext_->bml = bml;
  } else if (rrc->type == DELETE_REPLY) {
    auto bml = allocatorProxy_->get_cached_chunk(rrc->key);
    for (auto bm : bml) {
      rrc->success = allocatorProxy_->release(bm.address);
      if (rrc->success) {
        break;
      }
    }
    allocatorProxy_->del_chunk(rrc->key);
  } else {
  }
  requestReply->encode();
  networkServer_->send(reinterpret_cast<char *>(requestReply->data_),
                       requestReply->size_, rrc->con);
}

void Protocol::enqueue_rma_msg(uint64_t buffer_id) {
  std::unique_lock<std::mutex> lk(rrcMtx_);
  auto requestReply = rrcMap_[buffer_id];
  lk.unlock();
  std::shared_ptr<RequestReplyContext> rrc = requestReply->get_rrc();
  if (rrc->address != 0) {
    auto wid = GET_WID(rrc->address);
    readWorkers_[wid]->addTask(requestReply);
  } else {
    readWorkers_[rrc->rid % config_->get_pool_size()]->addTask(requestReply);
  }
}

void Protocol::handle_rma_msg(std::shared_ptr<RequestReply> requestReply) {
  std::shared_ptr<RequestReplyContext> rrc = requestReply->get_rrc();
  switch (rrc->type) {
    case WRITE_REPLY: {
      char *buffer = static_cast<char *>(rrc->ck->buffer);
      if (rrc->address == 0) {
        rrc->address = allocatorProxy_->allocate_and_write(
            rrc->size, buffer, rrc->rid % config_->get_pool_size());
      } else {
        allocatorProxy_->write(rrc->address, buffer, rrc->size);
      }
      networkServer_->reclaim_dram_buffer(rrc);
      break;
    }
    case READ_REPLY: {
      networkServer_->reclaim_pmem_buffer(rrc);
      break;
    }
    case PUT_REPLY: {
      char *buffer = static_cast<char *>(rrc->ck->buffer);
      assert(rrc->address == 0);
#ifdef DEBUG
      std::cout << "[handle_rma_msg]" << rrc->src_rkey << "-"
                << rrc->src_address << ":" << rrc->size << std::endl;
#endif
      rrc->address = allocatorProxy_->allocate_and_write(
          rrc->size, buffer, rrc->rid % config_->get_pool_size());
      networkServer_->reclaim_dram_buffer(rrc);
      break;
    }
    default: { break; }
  }
  enqueue_finalize_msg(requestReply);
}