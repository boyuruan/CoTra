#pragma once

// Template implementation for ScalaSearch; included after its declaration.
namespace hnswlib {

template <typename dist_t>
void ScalaSearch<dist_t>::do_task(ComputeTask<dist_t> &task) {
#ifdef DEBUG
  printf("do task %u\n", task.qid);
#endif
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
  m_profiler.start("g_do_task");
#endif
  _mm_prefetch(global_query[task.qid]->vector, _MM_HINT_T0);
  char *vec_ptr = graph_index.scala_graph->vector_ptr;
  uint32_t part_ofs = graph_index.scala_graph->scala_graph_get_partition_ofs();
  uint32_t *ngh_arr =
      graph_index.scala_graph->scala_graph_v3_get_ngh_ptr(task.offset);
  _mm_prefetch((char *)ngh_arr, _MM_HINT_T0);
  _mm_prefetch(
      vec_ptr + ((*(ngh_arr)) - part_ofs) * data_label_size, _MM_HINT_T0);
  _mm_prefetch((char *)(ngh_arr + 1), _MM_HINT_T0);
  worklists::Chunk<std::pair<dist_t, uint32_t>, MAX_GRAPH_DEG> chunk(
      task.is_count);
#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif
  for (uint32_t d = 0; d < task.deg; d++) {
    _mm_prefetch(
        vec_ptr + ((*(ngh_arr + d + 1)) - part_ofs) * data_label_size,
        _MM_HINT_T0);
    // TODO: may have problem with concurrent
    if (!global_query[task.qid]->visit_hash.CheckAndSet(*(ngh_arr + d))) {
#ifdef PROF_COMPUTATION
      computation_cnt++;
#endif
#ifdef COMM_PROFILE
      comm_ctx->work_dist++;
#endif
      char *vec_data =
          vec_ptr + ((*(ngh_arr + d)) - part_ofs) * data_label_size;
      dist_t dist = fstdistfunc_(
          global_query[task.qid]->vector, vec_data, dist_func_param_);
      chunk.emplace_back(dist, *(ngh_arr + d));
    }
  }
  global_query[task.qid]->con_task_queue.push(chunk);
#ifdef PROFILER
  m_profiler.end("g_do_task");
#endif
#ifdef DEBUG
  printf("do task %u over\n", task.qid);
#endif
  return;
}

template <typename dist_t>
void ScalaSearch<dist_t>::work_steal() {
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
  m_profiler.start("g_steal");
#endif
  // Use work stealing across threads.
  auto task = ba_task_queue.pop();
  if (task.has_value()) {
    do_task(task.value());
  }
#ifdef PROFILER
  m_profiler.end("g_steal");
#endif
}

template <typename dist_t>
void ScalaSearch<dist_t>::merge_completed_work(size_t query_id) {
  _mm_prefetch(global_query[query_id]->vector, _MM_HINT_T0);
  process_local_neighbor_tasks(query_id);
  process_shared_neighbor_tasks(query_id);
  process_inline_neighbor_tasks(query_id);
  process_remote_reads(query_id);
  process_remote_results(query_id);
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_local_neighbor_tasks(size_t query_id) {
  char *vec_ptr = graph_index.scala_graph->vector_ptr;
  uint32_t part_ofs = graph_index.scala_graph->scala_graph_get_partition_ofs();
#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif
  if (global_query[query_id]->local_task.size()) {
#ifdef DEBUG
    printf("proc qlocal task\n");
#endif
    for (ComputeTask<dist_t> &task : global_query[query_id]->local_task) {
      uint32_t *ngh_arr =
          graph_index.scala_graph->scala_graph_v3_get_ngh_ptr(task.offset);
      _mm_prefetch((char *)ngh_arr, _MM_HINT_T0);
      _mm_prefetch(
          vec_ptr + ((*(ngh_arr)) - part_ofs) * data_label_size, _MM_HINT_T0);
      _mm_prefetch((char *)(ngh_arr + 1), _MM_HINT_T0);
      for (uint32_t d = 0; d < task.deg; d++) {
        _mm_prefetch(
            vec_ptr + ((*(ngh_arr + d + 1)) - part_ofs) * data_label_size,
            _MM_HINT_T0);
        if (!global_query[query_id]->visit_hash.CheckAndSet(*(ngh_arr + d))) {
#ifdef PROF_COMPUTATION
          computation_cnt++;
#endif
#ifdef COMM_PROFILE
          comm_ctx->work_dist++;
#endif
          char *vec_data =
              vec_ptr + ((*(ngh_arr + d)) - part_ofs) * data_label_size;
          dist_t dist = fstdistfunc_(
              global_query[query_id]->vector, vec_data, dist_func_param_);
          CandidateSet<dist_t>::accept_local(
              *global_query[query_id], dist, *(ngh_arr + d));
        }
      }
    }
    global_query[query_id]->local_task.clear();
#ifdef DEBUG
    printf("proc qlocal task over\n");
#endif
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_shared_neighbor_tasks(size_t query_id) {
  auto &recv_cnt = global_query[query_id]->recv_cnt;
  for (;;) {
    auto task = ba_task_queue.pop(false);
    if (!task.has_value()) {
      break;
    }
    // Use work stealing across threads.
    do_task(task.value());
  }

  for (;;) {
    auto chunk = global_query[query_id]->con_task_queue.pop();
    if (!chunk.has_value()) {
      break;
    }
#ifdef DEBUG
    printf("chunk size: %u\n", chunk->size());
#endif
    recv_cnt += chunk.value().isCount();
    for (int i = 0; i < chunk.value().size(); i++) {
      auto &ele = chunk.value().getAt(i);
      CandidateSet<dist_t>::accept_local(
          *global_query[query_id], ele.first, ele.second);
    }
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_inline_neighbor_tasks(size_t query_id) {
  char *vec_ptr = graph_index.scala_graph->vector_ptr;
  uint32_t part_ofs = graph_index.scala_graph->scala_graph_get_partition_ofs();
#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif
  if (global_query[query_id]->local_micro_task.size()) {
#ifdef DEBUG
    printf("proc qlocal micro task\n");
#endif
    for (uint32_t i = 0; i < global_query[query_id]->local_micro_task.size();
         i++) {
      uint32_t &vid = global_query[query_id]->local_micro_task[i];
      // Existing look-ahead can read past the last ID; see refactor-notes.md.
      _mm_prefetch(
          vec_ptr +
              (global_query[query_id]->local_micro_task[i + 1] - part_ofs) *
                  data_label_size,
          _MM_HINT_T0);
#ifdef COMM_PROFILE
      comm_ctx->chan_deliv[CH_MICRO]++;
#endif
      if (!global_query[query_id]->visit_hash.CheckAndSet(vid)) {
#ifdef PROF_COMPUTATION
        computation_cnt++;
#endif
#ifdef COMM_PROFILE
        comm_ctx->work_dist++;
#endif
        char *vec_data = vec_ptr + (vid - part_ofs) * data_label_size;
        dist_t dist = fstdistfunc_(
            global_query[query_id]->vector, vec_data, dist_func_param_);
        bool took = CandidateSet<dist_t>::accept_local(
            *global_query[query_id], dist, vid);
#ifdef COMM_PROFILE
        comm_ctx->chan_pass[CH_MICRO] += took;
#endif
      } else {
#ifdef COMM_PROFILE
        comm_ctx->chan_dup[CH_MICRO]++;
#endif
      }
    }
    global_query[query_id]->local_micro_task.clear();
#ifdef DEBUG
    printf("proc qlocal micro task over\n");
#endif
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_remote_reads(size_t query_id) {
  char *vec_ptr = graph_index.scala_graph->vector_ptr;
  auto &recv_cnt = global_query[query_id]->recv_cnt;
#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif
  if (global_query[query_id]->recv_async_read_queue.size()) {
#ifdef DEBUG
    printf("proc async_read\n");
#endif
    for (BufferCache &r : global_query[query_id]->recv_async_read_queue) {
      uint32_t ngh_id = r.vector_id;
      char *vec_ptr = r.buffer_ptr;
      _mm_prefetch(vec_ptr, _MM_HINT_T0);
#ifdef COMM_PROFILE
      comm_ctx->chan_deliv[CH_READ]++;
#endif
      if (!global_query[query_id]->visit_hash.CheckAndSet(ngh_id)) {
#ifdef PROF_COMPUTATION
        computation_cnt++;
#endif
#ifdef COMM_PROFILE
        comm_ctx->work_dist++;
#endif
        dist_t dist = fstdistfunc_(
            global_query[query_id]->vector, vec_ptr, dist_func_param_);
        bool took = CandidateSet<dist_t>::accept_local(
            *global_query[query_id], dist, ngh_id);
#ifdef COMM_PROFILE
        comm_ctx->chan_pass[CH_READ] += took;
#endif
      } else {
#ifdef COMM_PROFILE
        comm_ctx->chan_dup[CH_READ]++;
#endif
      }
      rdma_comm.release_cache(r.buffer_id);
      // free(r);
      recv_cnt++;
    }
    global_query[query_id]->recv_async_read_queue.clear();
#ifdef DEBUG
    printf("proc async_read over\n");
#endif
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_remote_results(size_t query_id) {
  auto &recv_cnt = global_query[query_id]->recv_cnt;
  auto &nocore_recv = global_query[query_id]->nocore_recv;
#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif
  if (global_query[query_id]->local_res.size()) {
#ifdef DEBUG
    printf("proc qlocal_res\n");
#endif
    for (ResultMsg<dist_t> &result : global_query[query_id]->local_res) {
      for (std::pair<dist_t, uint32_t> &r : result.res) {
        dist_t dist = r.first;
        uint32_t candidate_id = r.second;
        bool took = CandidateSet<dist_t>::accept_local(
            *global_query[query_id], dist, candidate_id);
#ifdef COMM_PROFILE
        int ch = result.from_node ? CH_NODE_RESULT : CH_RESULT;
        comm_ctx->chan_deliv[ch]++;
        comm_ctx->chan_pass[ch] += took;
#endif
      }
      // count the res from core and nocore result.
      recv_cnt += result.core_cnt;
      nocore_recv += result.nocore_cnt;
    }
    global_query[query_id]->local_res.clear();
#ifdef DEBUG
    printf("proc qlocal_res over\n");
#endif
  }
}

}  // namespace hnswlib
