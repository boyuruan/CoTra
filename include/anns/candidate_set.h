#pragma once

#include "anns/query_state.h"

// Top-index acceptance and graph-search acceptance have distinct side effects.
// Synchronized candidates are handled separately in QuerySynchronization.
template <typename dist_t>
struct CandidateSet {
  static void accept_top(
      QueryState<dist_t> &query, dist_t dist, uint32_t candidate_id) {
    bool flag_consider_candidate =
        query.pre_top_cand.size() < query.ef || query.lowerBound > dist;

    if (flag_consider_candidate) {
      query.pre_cand_set.emplace(-dist, candidate_id);
      query.pre_top_cand.emplace(dist, candidate_id);

      while (query.pre_top_cand.size() > query.ef) {
        query.pre_top_cand.pop();
      }

      if (!query.pre_top_cand.empty())
        query.lowerBound = query.pre_top_cand.top().first;
    }
  }

  static bool accept_local(
      QueryState<dist_t> &query, dist_t dist, uint32_t candidate_id) {
    bool flag_consider_candidate =
        query.top_candidates.size() < query.ef || query.lowerBound > dist;

    if (flag_consider_candidate) {
      query.candidate_set.emplace(-dist, candidate_id);
      query.top_candidates.emplace(dist, candidate_id);

      // update sync msg.
      query.sync_msg.local_add_cand.emplace_back(dist, candidate_id);

      // do process, update local term
      query.proc_is_black |= 1;

      while (query.top_candidates.size() > query.ef) {
        uint32_t id = query.top_candidates.top().second;
        query.top_candidates.pop();
      }

      if (!query.top_candidates.empty())
        query.lowerBound = query.top_candidates.top().first;
    }
    return flag_consider_candidate;
  }
};
