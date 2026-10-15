#pragma once

// Template implementation for ScalaSearch; included after its declaration.
namespace hnswlib {

template <typename dist_t>
void ScalaSearch<dist_t>::search_top_index(size_t query_id) {
  TaskManager<dist_t> &tman = *task_ma.getLocal();
#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif
  global_query[query_id]->state = PRE_STAGE;

#ifdef PROFILER
  global_query[query_id]->profiler.start("q_pre-stage");
  global_query[query_id]->profiler.start("q_over_all");
#endif
  // in scala v3, cur_query_num record the number of query in pre stage.
  tman.cur_query_num++;
  uint32_t enterpoint = 0;
  // assume enter point is cached.
  char *ep_data = graph_index.top_index->get_vector(enterpoint);

  // Traverse on base index. (Use one-side migrate)
  dist_t dist =
      fstdistfunc_(global_query[query_id]->vector, ep_data, dist_func_param_);
#ifdef COMM_PROFILE
  comm_ctx->work_dist++;
#endif
  global_query[query_id]->lowerBound = dist;
  global_query[query_id]->pre_top_cand.emplace(dist, enterpoint);
  global_query[query_id]->pre_cand_set.emplace(-dist, enterpoint);

  uint32_t gt_id = graph_index.top_index->get_global_id(enterpoint);
  global_query[query_id]->visit_hash.CheckAndSet(gt_id);
  global_query[query_id]->ef = std::max((uint32_t)ef_, anns_param.res_knn);
  global_query[query_id]->is_new = false;

  // Pre-stage.
  while (!global_query[query_id]->pre_cand_set.empty()) {
    std::pair<dist_t, uint32_t> cand =
        global_query[query_id]->pre_cand_set.top();
    dist_t candidate_dist = -cand.first;
    if (candidate_dist > global_query[query_id]->lowerBound) {
      break;
    }
    global_query[query_id]->pre_cand_set.pop();

    _mm_prefetch(global_query[query_id]->vector, _MM_HINT_T0);
    uint32_t *ngh_arr = graph_index.top_index->get_ngh_arr(cand.second);
    _mm_prefetch((char *)ngh_arr, _MM_HINT_T0);

    _mm_prefetch((char *)(ngh_arr + 1), _MM_HINT_T0);
    for (uint32_t d = 0; d < ngh_arr[0]; d++) {
      _mm_prefetch(
          graph_index.top_index->get_vector(ngh_arr[d + 1]), _MM_HINT_T0);

      // Existing d-based access includes the degree slot. Preserve it here;
      // the suspected off-by-one is recorded in docs/refactor-notes.md.
      uint32_t gt_id = graph_index.top_index->get_global_id(ngh_arr[d]);
      if (!global_query[query_id]->visit_hash.CheckAndSet(gt_id)) {
#ifdef PROF_COMPUTATION
        computation_cnt++;
#endif
#ifdef COMM_PROFILE
        comm_ctx->work_dist++;
#endif
        char *vec_data = graph_index.top_index->get_vector(ngh_arr[d]);
        dist_t dist = fstdistfunc_(
            global_query[query_id]->vector, vec_data, dist_func_param_);
        CandidateSet<dist_t>::accept_top(
            *global_query[query_id], dist, ngh_arr[d]);
      }
    }
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::select_core_machines(size_t query_id) {
  /**
   * Previous stage over, start to dispatch sub-query.
   * First filterred the candidate set.
   */
  // TODO: maybe can use top_candidates to filter.
  memset(
      global_query[query_id]->filter_num, 0,
      sizeof(global_query[query_id]->filter_num));
  while (!global_query[query_id]->pre_top_cand.empty()) {
    auto cand_top = global_query[query_id]->pre_top_cand.top();
    uint32_t gt_id = graph_index.top_index->get_global_id(cand_top.second);
    int mid = graph_index.scala_graph->get_machine_id(gt_id);
    global_query[query_id]->filterred[mid].emplace_back(-cand_top.first, gt_id);
    global_query[query_id]->filter_num[mid]++;
    global_query[query_id]->top_candidates.emplace(cand_top.first, gt_id);
    global_query[query_id]->pre_top_cand.pop();
  }

  // Count
  uint32_t su_cnt = 0;
  uint32_t mx_cnt = 0, mx_mid;
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    su_cnt += global_query[query_id]->filter_num[m];
    if (global_query[query_id]->filter_num[m] > mx_cnt) {
      mx_cnt = global_query[query_id]->filter_num[m];
      mx_mid = m;
    }
  }
  global_query[query_id]->leader_machine = mx_mid;
#ifdef DEBUG
  printf("query %u leader machine is %u\n", query_id, mx_mid);
#endif

  // Identify which partition should be selected.

  // The leader is always core. On an exact even split no machine is above
  // the bar, nobody runs the query, and the issuer waits forever.
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    float ratio_m = (float)global_query[query_id]->filter_num[m] / su_cnt;
    if (ratio_m > 1.0 / (MACHINE_NUM) || m == mx_mid) {
      global_query[query_id]->core_machine.emplace_back(m);
      global_query[query_id]->is_core_machine[m] = 1;
    } else {
      global_query[query_id]->is_core_machine[m] = 0;
      // Move all non-core machine cand to the leader machine.
      for (auto &cand : global_query[query_id]->filterred[m]) {
        global_query[query_id]
            ->filterred[cotra::seed_target(
                m, mx_mid, global_query[query_id]->is_core_machine)]
            .emplace_back(cand);
      }
    }
  }
}

template <typename dist_t>
bool ScalaSearch<dist_t>::dispatch_subqueries(size_t query_id) {
  bool local_is_core = false;
  for (uint32_t &m : global_query[query_id]->core_machine) {
    if (m == rdma_param.machine_id) {
      // local machine.
      local_is_core = true;
      for (auto &cand : global_query[query_id]->filterred[m]) {
        global_query[query_id]->candidate_set.emplace(cand);
      }
    } else {
      protocol.fork_query(m, global_query[query_id]);
    }
  }
  global_query[query_id]->visit_hash.clear();

#ifdef PROFILER
  global_query[query_id]->profiler.end("q_pre-stage");
#endif

  return local_is_core;
}

}  // namespace hnswlib
