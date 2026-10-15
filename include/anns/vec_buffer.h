#pragma once
#include <stdlib.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>

#include "../rdma/rdma_param.h"
#include "anns/anns_param.h"
#include "anns/protocol/search_messages.h"

struct BufferCache {
  uint32_t machine_id;
  uint64_t vector_id;
  uint64_t internal_id;
  int buffer_id;
  char* buffer_ptr;
  BufferCache() {}

  BufferCache(
      uint32_t _machine_id, uint64_t _vector_id, uint64_t _internal_id,
      int _buffer_id, char* _buffer_ptr)
      : machine_id(_machine_id),
        vector_id(_vector_id),
        internal_id(_internal_id),
        buffer_id(_buffer_id),
        buffer_ptr(_buffer_ptr) {}
};

template <typename dist_t>
struct VectorCache {
  dist_t dist;
  uint64_t vector_id;
  int buffer_id;  // -1 means local buffer or not dispatch.
  char* ptr;
};

// TODO: ReadBuffer
struct VectorBuffer {
  size_t vec_len = MAX_VEC_LEN;
  std::deque<int> buff_id_list;
  char vector[MAX_CACHE_VEC][MAX_VEC_LEN];
  BufferCache bufcache_ptr[MAX_CACHE_VEC];
  size_t cur_buffer_id[MACHINE_NUM];
  std::deque<size_t> recv_list;
};

/**
 * Send Write Buffer
 */
struct SendWriteBuffer {
  size_t buffer_len = MAX_QUERYBUFFER_SIZE * MAX_WRITE_NUM;
  /*
    When used as send buffer, position_list[m] record the
    available id of buffer for machine m.
  */

  std::deque<uint32_t>* buff_id_list[MACHINE_NUM];

  char* buffer[MACHINE_NUM][MAX_WRITE_NUM];

  void init() {
    for (int i = 0; i < MACHINE_NUM; i++) {
      // TODO: may reserve for optimization.
      buff_id_list[i] = new std::deque<uint32_t>();
    }

    // TODO: may use numa alloc.
    char* buf_ptr = static_cast<char*>(
        malloc(MACHINE_NUM * MAX_WRITE_NUM * MAX_QUERYBUFFER_SIZE));

    std::fill(
        buf_ptr, buf_ptr + MACHINE_NUM * MAX_WRITE_NUM * MAX_QUERYBUFFER_SIZE,
        0);
    for (int i = 0; i < MACHINE_NUM; i++) {
      for (int j = 0; j < MAX_WRITE_NUM; j++) {
        buffer[i][j] = buf_ptr + (i * MAX_WRITE_NUM + j) * MAX_QUERYBUFFER_SIZE;
      }
    }
  }
};

/**
 * Recv Write Buffer
 */
struct RecvWriteBuffer {
  size_t buffer_len = MAX_QUERYBUFFER_SIZE * MAX_WRITE_NUM;
  /*
    When used as recv buffer, buff_id_list[m] record the
    meta data of received buffer for machine m.
  */
  std::deque<char*> part_info_queue;      // for build
  std::deque<char*> build_sync_queue;     // for build
  std::deque<char*> dispatch_meta_queue;  // for build
  std::deque<char*> dispatch_id_queue;    // for build
  std::deque<char*> dispatch_ngh_queue;   // for build

  std::deque<char*> scala_meta_queue;  // for scala meta
  std::deque<char*> scala_ngh_queue;   // for scala ngh

  std::deque<uint32_t>* release_id_list[MACHINE_NUM];
  // Collected migrate query pointer.
  // Transfer to general serialize and deserialize method in template class.
  std::deque<char*> migrate_queue;
  std::deque<char*> core_info_queue;
  std::deque<char*> sync_queue;
  std::deque<char*> recv_task;
  std::deque<char*> recv_result;
  std::deque<char*> recv_node_res;   // for node task
  std::deque<char*> recv_node_sync;  // for node task
  std::deque<char*> recv_compute;
  char* buffer[MACHINE_NUM][MAX_WRITE_NUM];

  void init(char* external_buf = nullptr) {
    for (int i = 0; i < MACHINE_NUM; i++) {
      release_id_list[i] = new std::deque<uint32_t>();
    }

    char* buf_ptr = external_buf;
    if (buf_ptr == nullptr) {
      buf_ptr = static_cast<char*>(
          malloc(MACHINE_NUM * MAX_WRITE_NUM * MAX_QUERYBUFFER_SIZE));

      std::fill(
          buf_ptr, buf_ptr + MACHINE_NUM * MAX_WRITE_NUM * MAX_QUERYBUFFER_SIZE,
          0);
    }
    for (int i = 0; i < MACHINE_NUM; i++) {
      for (int j = 0; j < MAX_WRITE_NUM; j++) {
        buffer[i][j] = buf_ptr + (i * MAX_WRITE_NUM + j) * MAX_QUERYBUFFER_SIZE;
      }
    }
  }
};
