#pragma once

// Template implementation for ScalaSearch; included after its declaration.
namespace hnswlib {

template <typename dist_t>
Corobj<bool> ScalaSearch<dist_t>::run_query(size_t query_id) {
  TaskManager<dist_t> &tman = *task_ma.getLocal();
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
#endif

#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif

  std::pair<dist_t, uint32_t> group_nodes[CAND_GROUP_SIZE];
  if (global_query[query_id]->is_new) {
#ifdef DEBUG
    printf("start new query %u, Pre STAGE\n", query_id);
#endif
    search_top_index(query_id);

#ifdef DEBUG
    printf("Start to dispatch sub-query of %u\n", query_id);
#endif
    select_core_machines(query_id);

    // Dispatch sub-query: fork query to core machine.
    bool local_is_core = dispatch_subqueries(query_id);

    if (!local_is_core) {
#ifdef DEBUG
      printf("Local not core, new query %u Pre STAGE over\n", query_id);
#endif
      co_yield false;
      // next time wake up will be END state.
    } else {
#ifdef DEBUG
      printf("Local is core, new query %u Pre STAGE over\n", query_id);
#endif
      if (global_query[query_id]->leader_machine == rdma_param.machine_id) {
        // local is leader, give the token.
        global_query[query_id]->has_token = true;
      }
    }

  } else {
#ifdef DEBUG
    printf("recv fork sub-query q%u\n", query_id);
#endif
    global_query[query_id]->state = PRE_STAGE;
  }

  // if local machine is core machine.
  if (global_query[query_id]->state == PRE_STAGE) {
    // Post stage: after dispatching sub-query.
#ifdef DEBUG
    printf("query %u goto post stage\n", query_id);
#endif
    global_query[query_id]->state = POST_STAGE;
    bool all_result_recved = false;
    auto &post_cnt = global_query[query_id]->post_cnt;
    auto &recv_cnt = global_query[query_id]->recv_cnt;
    auto &nocore_post = global_query[query_id]->nocore_post;
    auto &nocore_recv = global_query[query_id]->nocore_recv;
    post_cnt = recv_cnt = 0;
    nocore_post = nocore_recv = 0;

    while (!global_query[query_id]->global_term) {
      for (;;) {
#ifdef PROFILER
        m_profiler.start("post_task");
#endif
        int gsz = select_candidate_batch(query_id, group_nodes);
        // post micro task to remote machine.
        if (gsz > 0) {
          expand_candidate_batch(query_id, tman, group_nodes, gsz);
        } else {
          all_result_recved =
              (recv_cnt == post_cnt) && (nocore_post == nocore_recv);

#ifdef PROFILER
          m_profiler.end("post_task");
#endif
          if (!all_result_recved) {
            tman.next_subquery_queue.push_back(query_id);
          } else if (
              all_result_recved &&
              global_query[query_id]->state == POST_STAGE) {
            break;
          }
        }

        // Resume after the runtime has delivered completed work.
        co_yield false;
#ifdef PROFILER
        m_profiler.start("post_stage");
#endif

        merge_completed_work(query_id);

#ifdef PROFILER
        m_profiler.end("post_stage");
#endif
      }

      if (advance_termination(query_id)) break;
    }
  }
  assemble_result(query_id);

  // migrate to origin machine
  if (global_query[query_id]->leader_machine == rdma_param.machine_id &&
      rdma_param.machine_id != global_query[query_id]->origin_machine) {
    protocol.return_query_result(
        global_query[query_id]->origin_machine, global_query[query_id]);
    co_yield false;
  }
#ifdef PROFILER
  global_query[query_id]->profiler.end("q_over_all");
#endif
  co_yield true;
}

template <typename dist_t>
int ScalaSearch<dist_t>::select_candidate_batch(
    size_t query_id, std::pair<dist_t, uint32_t> *group_nodes) {
  int gsz = 0;
  for (gsz = 0;
       gsz < CAND_GROUP_SIZE && !global_query[query_id]->candidate_set.empty();
       gsz++) {
    group_nodes[gsz] = global_query[query_id]->candidate_set.top();
    dist_t candidate_dist = -group_nodes[gsz].first;
    if (candidate_dist > global_query[query_id]->lowerBound) {
      break;
    }
    global_query[query_id]->candidate_set.pop();
    // do not release here, since may in top candidates queue.
  }
  return gsz;
}

template <typename dist_t>
void ScalaSearch<dist_t>::expand_candidate_batch(
    size_t query_id, TaskManager<dist_t> &tman,
    std::pair<dist_t, uint32_t> *group_nodes, int gsz) {
  auto &post_cnt = global_query[query_id]->post_cnt;
#ifdef COMM_PROFILE
  RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif
#ifdef PROFILER
  Profiler &m_profiler = *all_profiler.getLocal();
#endif

#ifdef DEBUG
  printf("try get remote task\n");
#endif
  // Step 1: split remote and local node push local task to task
  // queue..
  for (int g = 0; g < gsz; g++) {
    tableint ngh_id = group_nodes[g].second;
    uint32_t mid = graph_index.scala_graph->get_machine_id(ngh_id);
    if (mid != rdma_param.machine_id) {
      // remote node task.
      tman.add_remote_task(ngh_id, mid, query_id);
      post_cnt++;
#ifdef COMM_PROFILE
      comm_ctx->work_node_task++;
#endif
    } else {
      // local node.
#ifdef COMM_PROFILE
      uint64_t *gated = comm_ctx->chan_gated;
      uint64_t *pq_dist = &comm_ctx->work_pq_dist;
      comm_ctx->work_expand++;
#else
      uint64_t *gated = nullptr;
      uint64_t *pq_dist = nullptr;
#endif
      post_cnt += neighbors.expand_local_node(
          ngh_id, query_id, tman, ba_task_queue, gated, pq_dist);
    }
  }
#ifdef DEBUG
  std::cout << "get remote task over, post\n";
#endif
  // Post micro task to non-core machine.
  protocol.post_remote_task(
      tman, query_id, data_size_ + sizeof(uint64_t),
      &global_query[query_id]->profiler);

  // sync msg & task to core machine.
  // untill end sync now.
  global_query[query_id]->sync_step++;
  if (global_query[query_id]->sync_step >= SYNC_BAR) {
    global_query[query_id]->sync_msg.lower_bound =
        global_query[query_id]->lowerBound;
    protocol.post_subquery_sync(global_query[query_id]);

    global_query[query_id]->sync_msg.clear();
    global_query[query_id]->sync_step = 0;
  }
#ifdef DEBUG
  std::cout << "remote task post over\n";
#endif

  // put subquery to next iteration global subquery queue.
  if (global_query[query_id]->state == POST_STAGE) {
    tman.next_subquery_queue.push_back(query_id);
  }
#ifdef PROFILER
  m_profiler.end("post_task");
#endif
}

}  // namespace hnswlib
