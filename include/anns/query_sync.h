#pragma once

#include "anns/query_state.h"

// Apply already-decoded updates; the runtime remains responsible for wakeups.
template <typename dist_t>
struct QuerySynchronization {
  static void apply_core(
      QueryState<dist_t>& query, uint32_t machine_id, SyncMsg<dist_t>* sync_msg,
      BalanceQueue<ComputeTask<dist_t>>* ib) {
    if (sync_msg->update_term) {
      // printf("q%u PROP update\n", sync_msg->qid);
      query.has_token = sync_msg->has_token;
      query.token_is_black = sync_msg->token_is_black;
    }

    // Sync top_candidate set & lower bound.
    for (std::pair<dist_t, uint32_t>& cand : sync_msg->local_add_cand) {
      if (query.top_candidates.size() < query.ef ||
          cand.first < query.lowerBound) {
        query.top_candidates.push(cand);
        // printf("--update d%llu v%u\n", cand.second, cand.first);
      }
    }
    while (query.top_candidates.size() > query.ef) {
      query.top_candidates.pop();
    }
    if (!query.top_candidates.empty())
      query.lowerBound = query.top_candidates.top().first;

    for (uint32_t& elem : sync_msg->core_micro_ngh[machine_id]) {
      query.local_micro_task.emplace_back(elem);
    }

    // Put into local queue.
    for (std::pair<size_t, uint32_t>& elem :
         sync_msg->core_large_ngh[machine_id]) {
      query.local_task.emplace_back(
          machine_id, query.query_id, elem.second, elem.first);
    }

    return;
  }

  static void apply_noncore(
      QueryState<dist_t>& query, uint32_t machine_id,
      NonCoreSyncMsg<dist_t>* sync_msg) {
    // printf("update sub query from noncore\n");
    for (uint32_t& elem : sync_msg->core_micro_ngh) {
      query.local_micro_task.emplace_back(elem);
    }

    // Put into local queue.
    for (std::pair<size_t, uint32_t>& elem : sync_msg->core_large_ngh) {
      query.local_task.emplace_back(
          machine_id, query.query_id, elem.second, elem.first);
    }

    return;
  }
};
