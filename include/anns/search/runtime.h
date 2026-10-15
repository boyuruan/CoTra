#pragma once

// Template implementation for ScalaSearch; included after its declaration.
namespace hnswlib {

template <typename dist_t>
void ScalaSearch<dist_t>::sendTerm(
    size_t correct, size_t total, size_t query_load, double cumu_lat_us) {
#ifdef PROF_COMPUTATION
  rdma_comm.send_term(
      rdma_param.machine_id, correct, total, query_load, cumu_lat_us,
      computation_cnt);
  computation_cnt = 0;
#else
  rdma_comm.send_term(
      rdma_param.machine_id, correct, total, query_load, cumu_lat_us, 0);
#endif
  return;
}

template <typename dist_t>
bool ScalaSearch<dist_t>::check_term() {
  rdma_comm.poll_send();
  rdma_comm.poll_recv();
  if (rdma_comm.check_term()) {
    rdma_comm.reset_sendrecv();
    return true;
  }

  return false;
}

template <typename dist_t>
void ScalaSearch<dist_t>::initTerm() {
  rdma_comm.reset_term();
}

template <typename dist_t>
void ScalaSearch<dist_t>::process_events(
    TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret) {
  process_query_messages(tman, ret);
  process_node_tasks(tman, ret);
  process_node_results(tman, ret);
  process_compute_tasks(tman, ret);
  process_read_completions(tman, ret);
  process_result_messages(tman, ret);
  resume_subqueries(tman, ret);
}

template <typename dist_t>
std::vector<QueryState<dist_t> *> ScalaSearch<dist_t>::submit_query(
    size_t qid) {
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
  m_profiler.start("g_overall");
#endif

  std::vector<QueryState<dist_t> *> ret;
  TaskManager<dist_t> &tman = *task_ma.getLocal();

  global_query[qid]->coro = run_query(qid);

  auto &task_queue = tman.get_local_task_queue();
  auto &node_queue = tman.get_node_task_queue();
  auto &result_queue = tman.get_result_queue();
  auto &query_queue = tman.get_query_queue();  // migrate query queue
  auto &async_queue = tman.get_async_task_queue();
  auto &subquery_queue = tman.get_subquery_queue();

  for (;;) {
    // put new (next) local task to cur task queue.
    tman.swap_task_queue();
    // collect task from other machine

#ifdef PROFILER
    protocol.poll_task_result(tman, &m_profiler);
#else
    protocol.poll_task_result(tman);
#endif

    if (!task_queue.size() && !result_queue.size() && !query_queue.size() &&
        !async_queue.size() && !subquery_queue.size() && !node_queue.size()) {
      if (tman.cur_query_num < QUERY_GROUP_SIZE) {
// all task is empty, add new query.
#ifdef PROFILER
        m_profiler.end("g_overall");
#endif
        return ret;
      } else {
        work_steal();
      }
    } else {
      process_events(tman, ret);
    }
  }

  // Should never be here.
  return ret;
}

template <typename dist_t>
std::vector<QueryState<dist_t> *> ScalaSearch<dist_t>::drain_queries() {
  std::vector<QueryState<dist_t> *> ret;
  auto &tman = *task_ma.getLocal();
  auto &task_queue = tman.get_local_task_queue();
  auto &node_queue = tman.get_node_task_queue();
  auto &result_queue = tman.get_result_queue();
  auto &query_queue = tman.get_query_queue();  // migrate query queue
  auto &async_queue = tman.get_async_task_queue();
  auto &subquery_queue = tman.get_subquery_queue();

  tman.swap_task_queue();
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
  protocol.poll_task_result(tman, &m_profiler);
#else
  protocol.poll_task_result(tman);
#endif

  if (query_queue.size() || result_queue.size() || task_queue.size() ||
      async_queue.size() || subquery_queue.size() || node_queue.size()) {
    process_events(tman, ret);
  } else {
    work_steal();
  }

  return ret;
}

template <typename dist_t>
void ScalaSearch<dist_t>::serve_until_batch_complete(uint32_t query_num) {
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
  m_profiler.start("g_overall");
#endif
  std::vector<QueryState<dist_t> *> ret;
  auto &tman = *task_ma.getLocal();

  auto &task_queue = tman.get_local_task_queue();
  auto &node_queue = tman.get_node_task_queue();
  auto &result_queue = tman.get_result_queue();
  auto &query_queue = tman.get_query_queue();  // migrate query queue
  auto &async_queue = tman.get_async_task_queue();
  auto &subquery_queue = tman.get_subquery_queue();

  for (;;) {
    // put new (next) local task to cur task queue.
    if (check_term()) break;
    tman.swap_task_queue();

#ifdef PROFILER
    protocol.poll_task_result(tman, &m_profiler);
#else
    protocol.poll_task_result(tman);
#endif

    if (query_queue.size() || result_queue.size() || task_queue.size() ||
        async_queue.size() || subquery_queue.size() || node_queue.size()) {
      process_events(tman, ret);
    } else {
      work_steal();
    }
  }
#ifdef PROFILER
  m_profiler.end("g_overall");
#endif
}

}  // namespace hnswlib
