#pragma once

// Template implementation for ScalaSearch; included after its declaration.
namespace hnswlib {

template <typename dist_t>
void ScalaSearch<dist_t>::process_query_messages(
    TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret) {
  auto &query_queue = tman.get_query_queue();
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
#endif

  if (query_queue.size()) {
#ifdef PROFILER
    m_profiler.start("g_query_Q");
#endif
    for (auto qid : query_queue) {
      if (global_query[qid]->is_new) {
        // new query
        global_query[qid]->is_new = false;
        if (global_query[qid]->leader_machine == rdma_param.machine_id) {
          global_query[qid]->has_token = 1;
        }
        global_query[qid]->coro = run_query(qid);
      } else {
// end query
#ifdef PROFILER
        global_query[qid]->profiler.end("q_over_all");
#endif
        take_query_result(tman, ret, qid);
      }
    }
    query_queue.clear();
#ifdef PROFILER
    m_profiler.end("g_query_Q");
#endif
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_node_tasks(
    TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret) {
  auto &node_queue = tman.get_node_task_queue();
#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif

  if (node_queue.size()) {
#ifdef DEBUG
    std::cout << "proc node task\n";
#endif
    for (NodeTask &task : node_queue) {
      if (task.machine_id == anns_param.machine_id) {  // local task.
        std::cout << "Error: non core machine never recv local node task.\n";
        abort();
      } else {
        /*
          1. For original core machine, local compute, compute and post res &
          post_cnt. use NodeResult queue
          2. For remote other core machines msg, post as sync msg, use sync
          post queue.
          3. For remote non-core machine task, post as remote task,
          node res post_cnt++, use task post queue.
        */

        tman.node_result_queue[task.machine_id].emplace_back(task.qid);
        NodeResult<dist_t> &res_msg =
            tman.node_result_queue[task.machine_id].back();

        // dispatch node task.
        neighbors.expand_remote_node(tman, task, res_msg);

        // ResultMsg<dist_t> res_msg(task.qid);
        // Start to traverse local partition ngh, based on task offset got.
        if (task.offset) {
          uint32_t *ngh_arr =
              graph_index.scala_graph->scala_graph_v3_get_ngh_ptr(task.offset);
          char *vec_ptr = graph_index.scala_graph->vector_ptr;
          uint32_t part_ofs =
              graph_index.scala_graph->scala_graph_get_partition_ofs();
          _mm_prefetch(
              vec_ptr + ((*(ngh_arr)) - part_ofs) * data_label_size,
              _MM_HINT_T0);
          _mm_prefetch(global_query[task.qid]->vector, _MM_HINT_T0);
          _mm_prefetch((char *)(ngh_arr + 1), _MM_HINT_T0);
          for (uint32_t d = 0; d < task.deg; d++) {
            _mm_prefetch(
                vec_ptr + ((*(ngh_arr + d + 1)) - part_ofs) * data_label_size,
                _MM_HINT_T0);
            if (!global_query[task.qid]->visit_hash.CheckAndSet(ngh_arr[d])) {
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
              res_msg.res.emplace_back(dist, ngh_arr[d]);
            }
          }
        }
      }

      if (tman.node_result_queue[task.machine_id].size() >= POST_RES_MERGE) {
        protocol.post_node_result(tman, task.machine_id);
      }
      if (tman.noncore_sync_msg[task.machine_id].size() >=
          POST_NODESYNC_MERGE) {
        protocol.post_node_sync(tman, task.machine_id);
      }
    }
    /*
      Send NodeResult to origin machine (NodeResult queue).
      Send Sync to core machine (sync queue).
      Send Task to non-core machine (task queue).
    */
    // TODO: check if swap queue may have bugs.
    protocol.post_remote_res_task_sync(tman);
    node_queue.clear();
#ifdef DEBUG
    std::cout << "proc node task over\n";
#endif
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_node_results(
    TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret) {
  auto &node_res_queue = tman.get_node_result_queue();

  auto &subquery_queue = tman.get_subquery_queue();

  if (node_res_queue.size()) {
#ifdef DEBUG
    std::cout << "proc node res\n";
#endif
    for (NodeResult<dist_t> &res_msg : node_res_queue) {
      if (global_query[res_msg.qid]->state == END) {
        printf("Error: q%u is END when recv res\n", res_msg.qid);
        abort();
      }
      // TODO: can polish here.
      ResultMsg<dist_t> tmp_res_msg(res_msg.qid);
      tmp_res_msg.res = res_msg.res;
      tmp_res_msg.from_node = 1;
      // Add new post count number.

      global_query[res_msg.qid]->nocore_post += res_msg.post_cnt;
      global_query[res_msg.qid]->local_res.push_back(tmp_res_msg);
      if (global_query[res_msg.qid]->state == PAUSE) {
        subquery_queue.push_back(res_msg.qid);
        global_query[res_msg.qid]->state = POST_STAGE;
      }
    }
    node_res_queue.clear();
#ifdef DEBUG
    std::cout << "proc node res over\n";
#endif
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_compute_tasks(
    TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret) {
  auto &task_queue = tman.get_local_task_queue();
#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif

  auto &result_queue = tman.get_result_queue();
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
#endif

  if (task_queue.size()) {
#ifdef DEBUG
    std::cout << "proc task queue\n";
#endif
#ifdef PROFILER
    m_profiler.start("g_task_Q");
#endif
    for (ComputeTask<dist_t> &task : task_queue) {
      if (task.machine_id == anns_param.machine_id) {  // local task.
        global_query[task.qid]->cur_task = &task;
        global_query[task.qid]->has_cur_task = true;
#ifdef DEBUG
        printf("local task\n");
#endif
        if (!global_query[task.qid]->result_taken &&
            global_query[task.qid]->coro()) {
          take_query_result(tman, ret, task.qid);
        }

      } else {
        //           // printf("start q%u task_remote timer\n", task.qid);
        tman.result_queue[task.machine_id].emplace_back(
            task.qid, task.core_cnt, task.nocore_cnt);
        ResultMsg<dist_t> &res_msg = tman.result_queue[task.machine_id].back();
        // ResultMsg<dist_t> res_msg(task.qid);
        uint32_t *ngh_arr;
        // uint32_t mptr[INLINE_DEG_BAR];
#ifdef DEBUG
        printf("remote task q%u deg:%u\n", task.qid, task.deg);
#endif
        if (task.deg <= INLINE_DEG_BAR) {
          // for (uint32_t d = 0; d < task.deg; d++) {
          //   // tmp_ptr[d] = vid;
          //   // printf("task.offset %llu\n", task.offset);
          //   mptr[d] = (uint32_t)(task.offset & 0xffffffff);
          //   task.offset >>= 32LL;
          // }
          // ngh_arr = mptr;

          ngh_arr = (uint32_t *)(&task.offset);
        } else {
          ngh_arr =
              graph_index.scala_graph->scala_graph_v3_get_ngh_ptr(task.offset);
        }

        char *vec_ptr = graph_index.scala_graph->vector_ptr;
        uint32_t part_ofs =
            graph_index.scala_graph->scala_graph_get_partition_ofs();
        _mm_prefetch(
            vec_ptr + ((*(ngh_arr)) - part_ofs) * data_label_size, _MM_HINT_T0);
        _mm_prefetch(global_query[task.qid]->vector, _MM_HINT_T0);
        _mm_prefetch((char *)(ngh_arr + 1), _MM_HINT_T0);
        for (uint32_t d = 0; d < task.deg; d++) {
          // if (task.deg <= INLINE_DEG_BAR) {
          // }
          _mm_prefetch(
              vec_ptr + ((*(ngh_arr + d + 1)) - part_ofs) * data_label_size,
              _MM_HINT_T0);
          if (!global_query[task.qid]->visit_hash.CheckAndSet(ngh_arr[d])) {
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
            // ef_fill: the issuer's top-ef was not full, or the task came
            // from node expansion and carries no bound. Keep everything.
            if (task.ef_fill || dist < task.lower_bound) {
              res_msg.res.emplace_back(dist, ngh_arr[d]);
            }
          }
        }
        if (tman.result_queue[task.machine_id].size() >= POST_RES_MERGE) {
          protocol.post_remote_result(tman, task.machine_id);
        }
      }
    }
    protocol.post_remote_result(tman);
    task_queue.clear();
#ifdef PROFILER
    m_profiler.end("g_task_Q");
#endif
#ifdef DEBUG
    std::cout << "proc task queue over\n";
#endif
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_read_completions(
    TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret) {
  auto &async_queue = tman.get_async_task_queue();
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
#endif

  if (async_queue.size()) {
#ifdef DEBUG
    printf("proc async queue\n");
#endif
#ifdef PROFILER
    m_profiler.start("g_async_Q");
#endif
    for (size_t &q : async_queue) {
      // all async read have finished.
      global_query[q]->has_cur_async = true;

      if (!global_query[q]->result_taken && global_query[q]->coro()) {
        take_query_result(tman, ret, q);
      }
    }
    async_queue.clear();
#ifdef DEBUG
    printf("proc async queue over\n");
#endif
#ifdef PROFILER
    m_profiler.end("g_async_Q");
#endif
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_result_messages(
    TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret) {
  auto &result_queue = tman.get_result_queue();
  auto &subquery_queue = tman.get_subquery_queue();
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
#endif

  if (result_queue.size()) {
#ifdef PROFILER
    m_profiler.start("g_result_Q");
#endif
#ifdef DEBUG
    printf("proc result queue\n");
#endif
    for (ResultMsg<dist_t> &res_msg : result_queue) {
      // if query is core, dont voke.
      if (global_query[res_msg.qid]->state == PRE_STAGE) {
        global_query[res_msg.qid]->cur_res = &res_msg;
        global_query[res_msg.qid]->has_cur_res = true;
        if (!global_query[res_msg.qid]->result_taken &&
            global_query[res_msg.qid]->coro()) {
          take_query_result(tman, ret, res_msg.qid);
        }
      } else {
        if (global_query[res_msg.qid]->state == END) {
          printf("WARN: q%u is END when recv res\n", res_msg.qid);
          abort();
        }
        global_query[res_msg.qid]->local_res.push_back(res_msg);
        if (global_query[res_msg.qid]->state == PAUSE) {
          subquery_queue.push_back(res_msg.qid);
          global_query[res_msg.qid]->state = POST_STAGE;
        }
      }
    }
    result_queue.clear();
#ifdef DEBUG
    printf("proc res over\n");
#endif
#ifdef PROFILER
    m_profiler.end("g_result_Q");
#endif
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::resume_subqueries(
    TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret) {
  auto &subquery_queue = tman.get_subquery_queue();
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
#endif

  if (subquery_queue.size()) {
#ifdef PROFILER
    m_profiler.start("g_subquery_Q");
#endif
    for (uint32_t &sqid : subquery_queue) {
      if (!global_query[sqid]->result_taken && global_query[sqid]->coro()) {
        take_query_result(tman, ret, sqid);
      }
    }
    subquery_queue.clear();
#ifdef PROFILER
    m_profiler.end("g_subquery_Q");
#endif
  }
}

}  // namespace hnswlib
