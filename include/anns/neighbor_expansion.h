#pragma once

#include "anns/pq_gate.h"
#include "anns/task_manager.h"
#include "index/scala_graph.h"
#include "worklists/balance_queue.h"

// Route graph neighbors without owning or changing the stored graph.
class NeighborExpansion {
 public:
  ScalaGraphIndex &graph;
  PqGate &pq_gate;

  NeighborExpansion(ScalaGraphIndex &graph_, PqGate &gate)
      : graph(graph_), pq_gate(gate) {}

  /**
   * Identify local core tasks, remote core tasks, remote non-core tasks.
   */
  template <typename dist_t>
  uint32_t expand_local_node(
      uint32_t current_node_id, uint32_t query_id, TaskManager<dist_t> &tman,
      BalanceQueue<ComputeTask<dist_t>> &ib, uint64_t *gated = nullptr,
      uint64_t *pq_dist_cnt = nullptr) {
    char *deg_ngh = graph.adjacency_data();
    if (current_node_id < graph.partition_ofs ||
        current_node_id >= graph.partition_ofs + graph.local_part_vec_num) {
      printf(
          "Error: current_node_id %u less or exceed partition_ofs %u\n",
          current_node_id, graph.partition_ofs);
      abort();
    }
    size_t ofs =
        graph.local_ngh_ele_size * (current_node_id - graph.partition_ofs);
    _mm_prefetch(deg_ngh + ofs, _MM_HINT_T0);
    uint32_t post_cnt = 0;
    dist_t lb = tman.global_query[query_id]->lowerBound;
    uint8_t ef_fill =
        (tman.global_query[query_id]->top_candidates.size() <
         tman.global_query[query_id]->ef);

    const char *qvec = tman.global_query[query_id]->vector;
    float pq_bound = (float)lb * pq_gate.scale;
    auto pq_drop = [&](uint32_t vid, int chan) {
      return pq_gate.drop(
          qvec, vid, ef_fill, pq_bound, chan, gated, pq_dist_cnt);
    };

    while (true) {
      const auto block = graph.neighbor_block(ofs);
      uint8_t end_flag = block.end_flag;
      uint8_t local_flag = block.local_flag;
      uint32_t pid = block.machine;
      uint32_t deg = block.degree;

      ofs += 2;
      if (local_flag) {
        // after previous stage, will put into local task queue.
        ib.emplace_back(graph.partition_id, query_id, deg, ofs);
        post_cnt++;
        // the local ngh must be end.
        break;
      } else {
        if (deg <= INLINE_DEG_BAR) {
          // micro task to other machine
          _mm_prefetch(deg_ngh + ofs + deg * 4, _MM_HINT_T0);
          uint32_t *ngh_ptr = (uint32_t *)(deg_ngh + ofs);
          if (tman.global_query[query_id]->is_core_machine[pid]) {
            // core ngh send to core machine
            for (uint32_t d = 0; d < deg; d++) {
              if (pq_drop(ngh_ptr[d], CH_MICRO)) continue;
              tman.global_query[query_id]
                  ->sync_msg.core_micro_ngh[pid]
                  .emplace_back(ngh_ptr[d]);
            }
          } else {  // async read to non-core machine
            for (uint32_t d = 0; d < deg; d++) {
              if (pq_drop(ngh_ptr[d], CH_READ)) continue;
              uint32_t internal_id =
                  graph.scala_graph_get_internal_id(pid, ngh_ptr[d]);
              tman.global_query[query_id]->async_read_queue.push_back(
                  BufferCache(pid, ngh_ptr[d], internal_id, -1, nullptr));
              // Need to post_cnt++ for micro async read.
              post_cnt++;
            }
          }
          ofs += deg * 4;
        } else {
          // high degree task to other machine
          _mm_prefetch(deg_ngh + ofs + 8, _MM_HINT_T0);
          uint64_t *ofs_ptr = (uint64_t *)(deg_ngh + ofs);
          if (tman.global_query[query_id]->is_core_machine[pid]) {
            // no need for post_cnt.
            tman.global_query[query_id]
                ->sync_msg.core_large_ngh[pid]
                .emplace_back(ofs_ptr[0], deg);
          } else {
            tman.task_queue[pid].emplace_back(
                graph.partition_id, query_id, deg, ofs_ptr[0], lb, ef_fill);
            post_cnt++;
          }
          ofs += 8;
        }
      }
      if (end_flag) {
        break;
      }
    }
    // post_cnt = number of async read vectors + number of remote tasks.
    return post_cnt;
  }

  /**
   * Identify local core tasks, remote core tasks, remote non-core tasks.
   */
  template <typename dist_t>
  void expand_remote_node(
      TaskManager<dist_t> &tman, NodeTask &task, NodeResult<dist_t> &res_msg) {
    char *deg_ngh = graph.adjacency_data();
    size_t ofs =
        graph.local_ngh_ele_size * (task.node_id - graph.partition_ofs);
    _mm_prefetch(deg_ngh + ofs, _MM_HINT_T0);
    res_msg.post_cnt = 0;

    while (true) {
      const auto block = graph.neighbor_block(ofs);
      uint8_t end_flag = block.end_flag;
      uint8_t local_flag = block.local_flag;
      uint32_t pid = block.machine;
      uint32_t deg = block.degree;
      ofs += 2;
      if (local_flag) {
        _mm_prefetch(deg_ngh + ofs + deg * 4, _MM_HINT_T0);
        // set local compute offset, will proc later.
        task.deg = deg;
        task.offset = ofs;
        break;
      } else {
        if (deg <= INLINE_DEG_BAR) {
          // micro task to other machine
          _mm_prefetch(deg_ngh + ofs + deg * 4, _MM_HINT_T0);
          uint32_t *ngh_ptr = (uint32_t *)(deg_ngh + ofs);
          if (task.is_core_machine[pid]) {
            // core ngh send to core machine
            tman.noncore_sync_msg[pid].emplace_back(task.qid);
            NonCoreSyncMsg<dist_t> &msg = tman.noncore_sync_msg[pid].back();
            for (uint32_t d = 0; d < deg; d++) {
              msg.core_micro_ngh.emplace_back(ngh_ptr[d]);
            }
          } else {  // send task to non-core machine
            uint64_t tmp_ofs_for_micro_combine = 0;
            for (uint32_t d = 0; d < deg; d++) {
              tmp_ofs_for_micro_combine |=
                  ((uint64_t)(ngh_ptr[d]) << (d * 32LL));
            }
            // NOTE: send back to origin machine (task.machine_id, not partition
            // id)
            tman.task_queue[pid].emplace_back(
                task.machine_id, task.qid, deg, tmp_ofs_for_micro_combine, 0, 1,
                0, 1);  // no bound: ef_fill 1; core count +0 nocore count +1
            res_msg.post_cnt++;
          }
          ofs += deg * 4;
        } else {
          // high degree task to other machine
          _mm_prefetch(deg_ngh + ofs + 8, _MM_HINT_T0);
          uint64_t *ofs_ptr = (uint64_t *)(deg_ngh + ofs);
          if (task.is_core_machine[pid]) {
            tman.noncore_sync_msg[pid].emplace_back(task.qid);
            NonCoreSyncMsg<dist_t> &msg = tman.noncore_sync_msg[pid].back();
            msg.core_large_ngh.emplace_back(ofs_ptr[0], deg);
          } else {
            // NOTE: send back to origin machine (task.machine_id, not partition
            // id)
            tman.task_queue[pid].emplace_back(
                task.machine_id, task.qid, deg, ofs_ptr[0], 0, 1, 0, 1);
            // no bound: ef_fill 1; core count +0 nocore count +1
            res_msg.post_cnt++;
          }
          ofs += 8;
        }
      }
      if (end_flag) {
        break;
      }
    }
    return;
  }
};
