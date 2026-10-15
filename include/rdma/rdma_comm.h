#pragma once
#include "anns/scala_scheduler.h"
#include "anns/task_manager.h"
#include "anns/vec_buffer.h"
#include "coromem/include/backend.h"
#include "coromem/include/galois/loops.h"
#include "index/index_param.h"
#include "rdma_context.h"

class RdmaCommunication {
 public:
  PerThreadStorage<RdmaContext> rdma_ctx;
  PerThreadStorage<VectorBuffer> read_buf;

  PerThreadStorage<SendWriteBuffer> send_write_buf;
  PerThreadStorage<RecvWriteBuffer> recv_write_buf;

  RdmaParameter rdma_param;

  // global query batch info.
  size_t vector_size;
  size_t correct{0}, total{0}, query_load{0};
  double query_lat_sum{0.0};
  // for comp eff profiling.
  size_t all_computation_cnt{0};

  RdmaCommunication() {}
  ~RdmaCommunication();

  void *shm_peer_base[MACHINE_NUM]{nullptr};
  size_t shm_peer_size[MACHINE_NUM]{0};
  std::string shm_my_path;
  void com_init_shm(char *vec_part_ptr, size_t vec_part_len);
  void shm_cleanup();

  // Indicator for termination.
  bool term[MACHINE_NUM];

  int ctx_close_connection();

  void com_init(
      char *vec_part_ptr, size_t vec_part_len, RdmaParameter &rdma_param);

  /**
   * Poll cq. Include send cq and recv cq.
   */
  int poll_send();
  int poll_recv();

  /**
   * Read operation. Include sync read and async read.
   */
  void com_read_init();
  char *sync_read(int machine_id, size_t internal_id, uint32_t size);
  int post_read(
      std::vector<BufferCache> &vectors, size_t vec_size,
      uint32_t query_id = 0);
  int poll_read();
  void release_cache(int buffer_id);
  char *fetch_vector(uint32_t internal_id, size_t machine_id, size_t vec_size);

  /**
   * Write operation. Include sync write and async write.
   */
  void com_write_init();
  int get_write_send_buf(
      int machine_id, bool send_release = false, bool pop = true);
  int post_write_send(
      int machine_id, uint32_t buf_id, uint32_t size, ControlType control);
  int post_write_recv(int machine_id, uint32_t buf_id);
  int post_sg_write_send(
      int machine_id, uint32_t buf_id, std::vector<char *> &sg_ptr,
      std::vector<uint32_t> &size, ControlType control);
  void send_write_release(int machine_id, uint32_t buf_id);
  void clear_write_release();

  void com_send_init() {}

  template <typename dist_t>
  void poll_task_result(
      TaskManager<dist_t> &tman, Profiler *profiler = nullptr) {
    auto *rbuf = recv_write_buf.getLocal();  // recv buffer
    auto *rd_buf = read_buf.getLocal();      // read buffer

    poll_send();
    poll_recv();

    // Collect recved tasks.
    while (rbuf->recv_task.size() > 0) {
#ifdef PROFILER
      profiler->start("g_poll_proc");
#endif
      char *task_ptr = rbuf->recv_task.front();
      rbuf->recv_task.pop_front();
      tman.deserialize_task(task_ptr);
      free(task_ptr);
#ifdef PROFILER
      profiler->end("g_poll_proc");
#endif
    }

    // Collect recved results.
    while (rbuf->recv_result.size() > 0) {
#ifdef PROFILER
      profiler->start("g_poll_proc");
#endif
      char *res_ptr = rbuf->recv_result.front();
      rbuf->recv_result.pop_front();
      tman.deserialize_result(res_ptr);
      free(res_ptr);
#ifdef PROFILER
      profiler->end("g_poll_proc");
#endif
    }

    while (rbuf->recv_node_res.size() > 0) {
      char *res_ptr = rbuf->recv_node_res.front();
      rbuf->recv_node_res.pop_front();
      tman.deserialize_node_result(res_ptr);
      free(res_ptr);
    }

    while (rbuf->recv_compute.size() > 0) {
#ifdef PROFILER
      profiler->start("g_poll_proc");
#endif
      char *cmp_ptr = rbuf->recv_compute.front();
      rbuf->recv_compute.pop_front();
      tman.deserialize_compute(cmp_ptr);
#ifdef PROFILER
      profiler->end("g_poll_proc");
#endif
    }

    // Collect migrated queries.
    while (rbuf->migrate_queue.size() > 0) {
#ifdef PROFILER
      profiler->start("g_poll_proc");
#endif
      char *query_ptr = rbuf->migrate_queue.front();
      rbuf->migrate_queue.pop_front();
      tman.deserialize_query(query_ptr);
      free(query_ptr);
#ifdef PROFILER
      profiler->end("g_poll_proc");
#endif
    }

    while (rbuf->core_info_queue.size() > 0) {
#ifdef PROFILER
      profiler->start("g_poll_proc");
#endif
      char *core_ptr = rbuf->core_info_queue.front();
      rbuf->core_info_queue.pop_front();
      tman.deserialize_core_info(core_ptr);
      free(core_ptr);
#ifdef PROFILER
      profiler->end("g_poll_proc");
#endif
    }

    size_t nsync_sz = rbuf->recv_node_sync.size();
    for (size_t i = 0; i < nsync_sz; i++) {
      char *sync_ptr = rbuf->recv_node_sync.front();
      rbuf->recv_node_sync.pop_front();
      if (!tman.can_apply_node_sync(sync_ptr)) {
        rbuf->recv_node_sync.push_back(sync_ptr);
        continue;
      }
      tman.deserialize_node_sync(sync_ptr);
      free(sync_ptr);
    }

    // Collect sub-queries sync msg.
    size_t sync_sz = rbuf->sync_queue.size();
    for (size_t i = 0; i < sync_sz; i++) {
#ifdef PROFILER
      profiler->start("g_poll_proc");
#endif
      char *sync_ptr = rbuf->sync_queue.front();
      rbuf->sync_queue.pop_front();
      if (!tman.can_apply_sync(sync_ptr)) {
        rbuf->sync_queue.push_back(sync_ptr);
        continue;
      }
      tman.deserialize_sync(sync_ptr);
      free(sync_ptr);
#ifdef PROFILER
      profiler->end("g_poll_proc");
#endif
    }

    // Async read list.
    while (rd_buf->recv_list.size() > 0) {
#ifdef PROFILER
      profiler->start("g_poll_proc");
#endif
      uint32_t qid = (rd_buf->recv_list.front() >> 16);
      uint32_t buf_id = (rd_buf->recv_list.front() & 0xffff);
      tman.global_query[qid]->async_read_cnt++;
      rd_buf->recv_list.pop_front();
      tman.global_query[qid]->recv_async_read_queue.push_back(
          rd_buf->bufcache_ptr[buf_id]);
      if (tman.global_query[qid]->state == PRE_STAGE) {
        if (tman.global_query[qid]->async_read_cnt ==
            tman.global_query[qid]->async_read_num) {
          tman.async_task_queue.emplace_back(qid);
        }
      } else if (tman.global_query[qid]->state == PAUSE) {
        tman.subquery_queue.push_back(qid);
        tman.global_query[qid]->state = POST_STAGE;
      }
#ifdef PROFILER
      profiler->end("g_poll_proc");
#endif
    }
  }

  template <typename dist_t>
  void migrate_query(size_t machine_id, QueryMsg<dist_t> *query) {
    auto *buf = send_write_buf.getLocal();
    uint32_t buf_id = get_write_send_buf(machine_id);
    // In scala_v3 a query migrates once, finished, back to its issuer.
#ifdef COMM_PROFILE
    uint64_t sec[4] = {0, 0, 0, 0};
    size_t len = query->serialize_end(buf->buffer[machine_id][buf_id], sec);
    count_query_sections(1, sec);
#else
    size_t len = query->serialize_end(buf->buffer[machine_id][buf_id]);
#endif
    post_write_send(machine_id, buf_id, len, QUERY);
  }

  // Section bytes of one QUERY message; purpose 0 is fork, 1 is migrate.
  void count_query_sections(int purpose, const uint64_t sec[4]) {
    auto *ctx = rdma_ctx.getLocal();
    ctx->query_cnt[purpose]++;
    ctx->query_head[purpose] += sec[0];
    ctx->query_top[purpose] += sec[1];
    ctx->query_cand[purpose] += sec[2];
    ctx->query_result[purpose] += sec[3];
  }

  /**
   * Fork query to other machine.
   */
  template <typename dist_t>
  void fork_query(size_t machine_id, QueryMsg<dist_t> *query) {
    auto *buf = send_write_buf.getLocal();
    uint32_t buf_id = get_write_send_buf(machine_id);
#ifdef COMM_PROFILE
    uint64_t sec[4] = {0, 0, 0, 0};
    size_t len = query->fork(buf->buffer[machine_id][buf_id], sec);
    count_query_sections(0, sec);
#else
    size_t len = query->fork(buf->buffer[machine_id][buf_id]);
#endif
    if (len >= MAX_QUERYBUFFER_SIZE) {
      printf("Error: query msg exceed the buffer size.\n");
      abort();
    }
    post_write_send(machine_id, buf_id, len, QUERY);
  }

  /**
   * Send query core machine info for scheduler.
   */
  template <typename dist_t>
  void send_core_machine_info(
      TaskManager<dist_t> &tman, QueryMsg<dist_t> *query) {
    uint32_t leader_m = 0;  // leader machine id.
    if (rdma_param.machine_id == leader_m) {
      tman.schedule_query_queue.emplace_back(
          query->query_id, query->core_machine);
    } else {
      auto *buf = send_write_buf.getLocal();
      uint32_t buf_id = get_write_send_buf(leader_m);
      ScheduleMsg msg(query->query_id, query->core_machine);
      size_t len = msg.serialize(buf->buffer[leader_m][buf_id]);
      post_write_send(leader_m, buf_id, len, CORE_INFO);
    }
  }

  /**
   * Send scheduler plan to other machine.
   */
  void send_schedule_plan(ScalaScheduler &scheduler) {}

  template <typename dist_t>
  void post_subquery_sync(
      QueryMsg<dist_t> *query, bool prop_token = false, bool taint = false) {
    auto *buf = send_write_buf.getLocal();
    uint32_t core_m_sz = query->core_machine.size();
    uint32_t nxt_prop_m;
    query->sync_msg.qid = query->query_id;

    for (uint32_t m_idx = 0; m_idx < core_m_sz; m_idx++) {
      if (query->core_machine[m_idx] == rdma_param.machine_id) {
        nxt_prop_m = query->core_machine[(m_idx + 1) % core_m_sz];
        break;
      }
    }
    for (uint32_t m : query->core_machine) {
      if (m == rdma_param.machine_id) {
        continue;
      } else {
        uint32_t buf_id = get_write_send_buf(m);

        if (prop_token && (nxt_prop_m == m) && query->has_token) {
          query->sync_msg.update_term = true;
          query->sync_msg.has_token = true;
          query->sync_msg.token_is_black = taint;
          query->has_token = 0;
        } else {
          query->sync_msg.update_term = 0;
          query->sync_msg.has_token = 0;
        }
#ifdef COMM_PROFILE
        uint64_t sec[3] = {0, 0, 0};
        size_t len = query->sync_msg.serialize(buf->buffer[m][buf_id], m, sec);
        auto *ctx = rdma_ctx.getLocal();
        ctx->sync_cnt++;
        ctx->sync_head += sec[0];
        ctx->sync_cand += sec[1];
        ctx->sync_target += sec[2];
        ctx->sync_token += query->sync_msg.update_term;
        ctx->sync_micro += query->sync_msg.core_micro_ngh[m].size();
        ctx->sync_large += query->sync_msg.core_large_ngh[m].size();
#else
        size_t len = query->sync_msg.serialize(buf->buffer[m][buf_id], m);
#endif
        post_write_send(m, buf_id, len, SYNC);
      }
    }
  }

  // For ScalaGraph task push.
  template <typename dist_t>
  void post_remote_task(
      TaskManager<dist_t> &tman, uint32_t query_id, size_t read_size,
      Profiler *profiler = nullptr) {
    auto *buf = send_write_buf.getLocal();

    // post async read
    tman.global_query[query_id]->async_read_num =
        tman.global_query[query_id]->async_read_queue.size();

    if (tman.global_query[query_id]->async_read_num > 0) {
      tman.global_query[query_id]->async_read_cnt = 0;
      post_read(
          tman.global_query[query_id]->async_read_queue, read_size, query_id);
      tman.global_query[query_id]->async_read_queue.clear();
    }

    // post write task
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == tman.local_id) continue;
      if (tman.task_queue[m].size() > 0 || tman.node_task_queue[m].size() > 0) {
#ifdef PROFILER
        profiler->count("task_push");
#endif
        uint32_t buf_id = get_write_send_buf(m);
        uint64_t offset = 0;
        tman.serialize_task(m, buf->buffer[m][buf_id], offset);
        post_write_send(m, buf_id, offset, TASK);
        tman.task_queue[m].clear();
        tman.node_task_queue[m].clear();
      }
    }
  }

  // For task push from node task.
  template <typename dist_t>
  void post_node_remote_task(TaskManager<dist_t> &tman) {
    auto *buf = send_write_buf.getLocal();

    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == tman.local_id) continue;
      if (tman.task_queue[m].size() > 0 || tman.node_task_queue[m].size() > 0) {
#ifdef PROFILER
        profiler->count("task_push");
#endif
        uint32_t buf_id = get_write_send_buf(m);
        uint64_t offset = 0;
        tman.serialize_task(m, buf->buffer[m][buf_id], offset);
        post_write_send(m, buf_id, offset, TASK);
        tman.task_queue[m].clear();
        tman.node_task_queue[m].clear();
      }
    }
  }

  template <typename dist_t>
  void post_result(ResultMsg<dist_t> &res_msg, uint32_t machine_id) {
    auto *buf = send_write_buf.getLocal();
    uint32_t buf_id = get_write_send_buf(machine_id);
    uint64_t offset = 0;
    res_msg.serialize(buf->buffer[machine_id][buf_id], offset);
    post_write_send(machine_id, buf_id, offset, RESULT);
  }

  template <typename dist_t>
  void post_compute(TaskManager<dist_t> &tman) {
    auto *buf = send_write_buf.getLocal();
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == tman.local_id) continue;
      if (tman.compute_queue[m].size() > 0) {
        for (ComputeMsg &cmp_msg : tman.compute_queue[m]) {
          uint64_t offset = 0;
          uint32_t buf_id = get_write_send_buf(m);
          tman.serialize_compute(cmp_msg, buf->buffer[m][buf_id], offset);
          post_sg_write_send(
              m, buf_id, cmp_msg.vec_ptr, cmp_msg.sg_size, COMPUTE);
        }
        tman.compute_queue[m].clear();
      }
    }
  }

  template <typename dist_t>
  void post_remote_result(TaskManager<dist_t> &tman, uint32_t machine_id) {
    auto *buf = send_write_buf.getLocal();
    uint32_t buf_id = get_write_send_buf(machine_id);
    uint64_t offset = 0;
    tman.serialize_result(machine_id, buf->buffer[machine_id][buf_id], offset);
    post_write_send(machine_id, buf_id, offset, RESULT);
    tman.result_queue[machine_id].clear();
  }

  template <typename dist_t>
  void post_remote_result(TaskManager<dist_t> &tman) {
    auto *buf = send_write_buf.getLocal();

    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == tman.local_id) continue;
      if (tman.result_queue[m].size() > 0) {
        uint32_t buf_id = get_write_send_buf(m);
        uint64_t offset = 0;
        tman.serialize_result(m, buf->buffer[m][buf_id], offset);
        post_write_send(m, buf_id, offset, RESULT);
        tman.result_queue[m].clear();
      }
    }
  }

  /**
   * Send node task result to origin machine. This machine must be non-core
   * machine. This func is called when the node result queue is full.
   */
  template <typename dist_t>
  void post_node_result(TaskManager<dist_t> &tman, uint32_t m) {
    auto *buf = send_write_buf.getLocal();
    if (tman.node_result_queue[m].size() > 0) {
      uint32_t buf_id = get_write_send_buf(m);
      uint64_t offset = 0;
      tman.serialize_node_result(m, buf->buffer[m][buf_id], offset);
      post_write_send(m, buf_id, offset, NODE_RESULT);
      tman.node_result_queue[m].clear();
    }
  }

  /**
   * Send node sync msg to non-core machine. This machine must be non-core
   * machine.
   */
  template <typename dist_t>
  void post_node_sync(TaskManager<dist_t> &tman, uint32_t m) {
    auto *buf = send_write_buf.getLocal();
    if (tman.noncore_sync_msg[m].size() > 0) {
      uint32_t buf_id = get_write_send_buf(m);
      uint64_t offset = 0;
      tman.serialize_node_sync(m, buf->buffer[m][buf_id], offset);
      post_write_send(m, buf_id, offset, NODE_SYNC);
      tman.noncore_sync_msg[m].clear();
    }
  }

  /**
   * Send node task result, task, sync msg to origin machine, coremachine,
   * non-core machine, respectively. This machine must be non-core machine.
   */
  template <typename dist_t>
  void post_remote_res_task_sync(TaskManager<dist_t> &tman) {
    auto *buf = send_write_buf.getLocal();
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == tman.local_id) continue;
      if (tman.node_result_queue[m].size() > 0) {
        uint32_t buf_id = get_write_send_buf(m);
        uint64_t offset = 0;
        tman.serialize_node_result(m, buf->buffer[m][buf_id], offset);
        post_write_send(m, buf_id, offset, NODE_RESULT);
        tman.node_result_queue[m].clear();
      }
    }

    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == tman.local_id) continue;
      if (tman.noncore_sync_msg[m].size() > 0) {
        uint32_t buf_id = get_write_send_buf(m);
        uint64_t offset = 0;
        tman.serialize_node_sync(m, buf->buffer[m][buf_id], offset);
        post_write_send(m, buf_id, offset, NODE_SYNC);
        tman.noncore_sync_msg[m].clear();
      }
    }

    post_node_remote_task(tman);
  }

  /**
   * Used for termination.
   */
  void send_term(
      int my_machine_id, size_t q_correct, size_t q_total,
      size_t q_query_load, double cumu_lat_us = 0.0,
      size_t computation_cnt = 0);
  bool check_term();
  void reset_term();
  void reset_sendrecv();

  /* This part is for index building. */
  void post_partition_info(IndexParameter &index_param, uint32_t machine_id);
  void poll_partition_info(IndexParameter &index_param);
  void build_sync();

  // For ScalaGraph.
  void swap_scala_index_meta_info(
      uint32_t machine_id, std::vector<size_t> &out_remote_ngh_num,
      std::vector<std::vector<size_t>> &in_remote_ngh_num);

  void swap_scalagraph_remote_ngh(
      std::vector<char *> &out_remote_ngh_ptr,
      std::vector<size_t> &out_remote_ngh_num, char *recv_ptr,
      size_t *in_remote_ngh_start_ofs,
      std::vector<std::vector<size_t>> &in_remote_ngh_num);
};
