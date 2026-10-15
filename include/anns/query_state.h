#pragma once

#include <atomic>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <unordered_set>

#include "anns_param.h"
#include "coromem/include/runtime/corobj.h"
#include "coromem/include/worklists/balance_queue.h"
#include "coromem/include/worklists/chunk.h"
#include "coromem/include/worklists/lockfreeQ.h"
#include "hash_table.h"
#include "rdma/profiler.h"
#include "vec_buffer.h"

/**
 * START: query init state.
 * PRE_STAGE: search the replicated top index and dispatch seeds.
 * POST_STAGE: after dispatch sub-query.
 * PAUSE: no local compute, wait for res/task or termination.
 * END: query end.
 */
typedef enum { START, PRE_STAGE, POST_STAGE, PAUSE, END } QueryPhase;

// Local query state; wire encoding lives in protocol/query_codec.h.
template <typename dist_t>
class QueryState {
 public:
  size_t ef;
  size_t query_id;
  size_t origin_machine;
  size_t current_node_id;
  dist_t lowerBound;
  OptHashPosVector visit_hash;

  Profiler profiler;

  size_t vector_size{0};
  char* vector{nullptr};
  QueryPhase state;

  // Path count, for migration ref.
  uint32_t path_cnt[MACHINE_NUM];

  // [TEMP] for profiling.
  uint32_t tmp_m_cnt[MACHINE_NUM];
  uint32_t tmp_path_cnt[MACHINE_NUM];

  uint32_t max_path_id{0}, max_path_cnt{0};
  uint32_t path_length{0};

  // for sub-query sync
  uint32_t leader_machine{0};
  uint8_t has_token{0}, token_is_black{0}, proc_is_black{1}, last_is_white{1};
  uint8_t global_term{0};

  std::vector<uint32_t> core_machine;
  uint32_t is_core_machine[MACHINE_NUM];

  // filterred candidates.
  std::vector<std::pair<dist_t, uint32_t>> filterred[MACHINE_NUM];
  uint32_t filter_num[MACHINE_NUM];

  uint32_t post_cnt{0}, recv_cnt{0};        // post/recv count.
  uint32_t nocore_post{0}, nocore_recv{0};  // no-core post/recv count.
  uint32_t sync_step{0};
  SyncMsg<dist_t> sync_msg;  // for sub-query sync.

  /**
   * After previous stage, local task will not put into global task queue
   * (tman).
   * local_task / local_micro_task: local genrated task & recved ngh sync from
   * other machines.
   */
  std::vector<ComputeTask<dist_t>> local_task;
  std::vector<uint32_t> local_micro_task;
  std::vector<ResultMsg<dist_t>> local_res;

  // indicate whether new (has coro) for this machine, forbid serialize.
  bool is_new{true};
  // Set once the finished query is handed to recall accounting.
  bool result_taken{false};

  // group cand nghs
  //   std::pair<dist_t, uint32_t> group_nodes[CAND_GROUP_SIZE];
  // corobj
  Corobj<bool> coro;

  bool has_cur_res = false;
  ResultMsg<dist_t>* cur_res;

  bool has_cur_cmp = false;
  ComputeMsg* cur_cmp;

  bool has_cur_task = false;
  ComputeTask<dist_t>* cur_task;

  bool has_cur_async = false;
  uint32_t async_read_num, async_read_cnt;
  std::vector<BufferCache> async_read_queue;       // pre stage only
  std::vector<BufferCache> recv_async_read_queue;  // post stage only

  LockFreeQueue<worklists::Chunk<std::pair<dist_t, uint32_t>, MAX_GRAPH_DEG>>
      con_task_queue;

  struct CompareByFirst {
    constexpr bool operator()(
        std::pair<dist_t, uint32_t> const& a,
        std::pair<dist_t, uint32_t> const& b) const noexcept {
      return a.first < b.first;
    }
  };

  std::priority_queue<
      std::pair<dist_t, uint32_t>, std::vector<std::pair<dist_t, uint32_t>>,
      CompareByFirst>
      top_candidates;
  std::priority_queue<
      std::pair<dist_t, uint32_t>, std::vector<std::pair<dist_t, uint32_t>>,
      CompareByFirst>
      candidate_set;

  // Used in top index (pre-stage).
  std::priority_queue<
      std::pair<dist_t, uint32_t>, std::vector<std::pair<dist_t, uint32_t>>,
      CompareByFirst>
      pre_top_cand;
  std::priority_queue<
      std::pair<dist_t, uint32_t>, std::vector<std::pair<dist_t, uint32_t>>,
      CompareByFirst>
      pre_cand_set;

  std::priority_queue<std::pair<dist_t, size_t>> result;

  QueryState() {}
  ~QueryState() { free(vector); }

  QueryState(QueryPhase _state) : state(_state), origin_machine(0) {}

  QueryState(
      char* _vector, size_t _vector_size, size_t _query_id, size_t _ef,
      size_t _machine)
      : vector_size(_vector_size),
        query_id(_query_id),
        ef(_ef),
        origin_machine(_machine),
        state(START) {
    vector = (char*)malloc(vector_size);
    memcpy(vector, _vector, vector_size);
    memset(path_cnt, 0, sizeof(path_cnt));
    memset(is_core_machine, 0, sizeof(is_core_machine));
  }

  void print_msg() {
    printf("ef %d\n", ef);
    printf("query_id %d\n", query_id);
    printf("current_node_id %d\n", current_node_id);
    printf("lowerBound %d\n", lowerBound);
    printf("top_candidates %d\n", top_candidates.size());
    printf("candidate_set %d\n", candidate_set.size());
    printf("result %d\n", result.size());
  }
};
