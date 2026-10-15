#pragma once

// Template implementation for ScalaSearch; included after its declaration.
namespace hnswlib {

template <typename dist_t>
bool ScalaSearch<dist_t>::advance_termination(size_t query_id) {
  // local termination, prop token
  if (global_query[query_id]->core_machine.size() > 1) {
    if (global_query[query_id]->has_token) {
      // if is leader
      if (global_query[query_id]->leader_machine == rdma_param.machine_id) {
        bool failed = global_query[query_id]->token_is_black ||
                      global_query[query_id]->proc_is_black;
        global_query[query_id]->token_is_black =
            global_query[query_id]->proc_is_black = false;
        if (global_query[query_id]->last_is_white && !failed) {
          // This was the second success
          global_query[query_id]->global_term = true;
          return true;
        }
        global_query[query_id]->last_is_white = !failed;
      }
      bool taint = global_query[query_id]->proc_is_black ||
                   global_query[query_id]->token_is_black;
      global_query[query_id]->proc_is_black =
          global_query[query_id]->token_is_black = false;

      // prop token.
      protocol.post_subquery_sync(global_query[query_id], true, taint);
      global_query[query_id]->sync_msg.clear();
      global_query[query_id]->has_token = false;

      global_query[query_id]->sync_step = 0;
    }
    global_query[query_id]->state = PAUSE;
  } else {
    // single core machine, just end.
    global_query[query_id]->global_term = true;
    return true;
  }
  return false;
}

template <typename dist_t>
void ScalaSearch<dist_t>::assemble_result(size_t query_id) {
  global_query[query_id]->state = END;
#ifdef DEBUG
  memset(
      global_query[query_id]->tmp_m_cnt, 0,
      sizeof(global_query[query_id]->tmp_m_cnt));
#endif
  while (global_query[query_id]->top_candidates.size() > anns_param.res_knn) {
#ifdef DEBUG
    auto tt = global_query[query_id]->top_candidates.top();
    int mid = graph_index.scala_graph->get_machine_id(tt.second);
    global_query[query_id]->tmp_m_cnt[mid]++;
#endif
    global_query[query_id]->top_candidates.pop();
  }

  while (global_query[query_id]->top_candidates.size() > 0) {
    std::pair<dist_t, tableint> rez =
        global_query[query_id]->top_candidates.top();
#ifdef DEBUG
    int mid = graph_index.scala_graph->get_machine_id(rez.second);
    global_query[query_id]->tmp_m_cnt[mid]++;
#endif
    global_query[query_id]->result.push(
        std::pair<dist_t, labeltype>(
            rez.first,
            graph_index.scala_graph->partition_map().label(rez.second)));
    global_query[query_id]->top_candidates.pop();
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::take_query_result(
    TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret,
    uint32_t qid) {
  QueryState<dist_t> *q = global_query[qid];
  if (q->result_taken) return;
  q->result_taken = true;
  ret.push_back(q);
  tman.cur_query_num--;
}

}  // namespace hnswlib
