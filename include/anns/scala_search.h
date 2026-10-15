#pragma once

#include <assert.h>
#include <stdlib.h>

#include <atomic>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <random>
#include <unordered_set>

#include "anns/candidate_set.h"
#include "anns/core_selection.h"
#include "anns/evaluation/search_metrics.h"
#include "anns/neighbor_expansion.h"
#include "anns/search_protocol.h"
#include "anns_param.h"
#include "coromem/include/backend.h"
#include "coromem/include/galois/loops.h"
#include "coromem/include/runtime/range.h"
#include "hash_table.h"
#include "index/graph_index.h"
#include "query_state.h"
#include "rdma/rdma_comm.h"
#include "scala_scheduler.h"
#include "task_manager.h"
#include "worklists/balance_queue.h"
#include "worklists/chunk.h"

namespace hnswlib {
typedef unsigned int tableint;
typedef unsigned int linklistsizeint;

template <typename dist_t>
class ScalaSearch : public AlgorithmInterface<dist_t> {
 public:
  size_t ef_{0};

  size_t data_size_{0};
  size_t data_label_size{0};

  cotra::SearchMetrics metrics;

  DISTFUNC<dist_t> fstdistfunc_;
  void *dist_func_param_{nullptr};

  AnnsParameter anns_param;
  GraphIndex &graph_index;
  PqGate pq_gate;
  NeighborExpansion neighbors{*graph_index.scala_graph, pq_gate};

  BalanceQueue<ComputeTask<dist_t>> ba_task_queue;

  bool should_term{false};
  bool has_send_term{false};
  QueryState<dist_t> **global_query;
  PerThreadStorage<Profiler> all_profiler;

  ScalaScheduler scheduler;

  bool haveInit = false;
  std::atomic<cycles_t> basetime{0};

  RdmaParameter rdma_param;
  RdmaCommunication rdma_comm;
  SearchProtocol protocol{rdma_comm};

  typedef QueryState<dist_t>::CompareByFirst CompareByFirst;

  char **remote_ptr;
  char *partition_ptr;
  size_t partition_size;

  size_t *remote_size;

  PerThreadStorage<TaskManager<dist_t>> task_ma{anns_param.machine_id};

#ifdef PROF_DEGREE
  uint64_t local_distri[MAX_GRAPH_DEG];
  uint64_t remote_distri[MAX_GRAPH_DEG];

  uint64_t local_cnt{0}, remote_cnt{0};
  uint64_t local_deg{0}, remote_deg{0};

#endif

#ifdef PROF_COMPUTATION
  // NOTE: no parallel, should be counted in single thread.
  // profiling shard index computation efficiency.
  size_t computation_cnt = 0;
#endif

#ifdef PROF_ALL_Q_DISTRI
  // NOTE: no parallel, should be counted in single thread.
  std::vector<size_t> all_q_distr_cnt = std::vector<size_t>(MACHINE_NUM, 0);
  std::vector<float> all_q_dist_cnt = std::vector<float>(MACHINE_NUM, 0.0);
#endif

  // set evaluation path
  void set_evaluation_save_path(std::string path) {
    metrics.set_evaluation_save_path(path);
  }

  std::ofstream &get_evaluation_save_file() {
    return metrics.evaluation_save_file;
  }

  // for all app type
  ScalaSearch(
      AnnsParameter anns_param_, RdmaParameter rdma_param_,
      SpaceInterface<dist_t> *space_interface, GraphIndex &graph_index_)
      : anns_param(anns_param_),
        rdma_param(rdma_param_),
        graph_index(graph_index_) {
    data_size_ = space_interface->get_data_size();
    data_label_size = data_size_ + sizeof(size_t);
    fstdistfunc_ = space_interface->get_dist_func();
    dist_func_param_ = space_interface->get_dist_func_param();
    if (data_size_ != graph_index.index_param.vec_size) {
      std::cerr << "Error: Mismatch vector data size. \n";
      abort();
    }

    global_query =
        (QueryState<dist_t> **)new QueryState<dist_t> *[anns_param.qsize]();
    printf("ScalaGraph init ...\n");
    graph_index.scala_graph->load_index();
    if (!anns_param.pq_prefix.empty()) init_pq_gate();
    run_rdma();
    printf("Init over ...\n");
  }

  PQCodes pq_codes;

  void init_pq_gate();

  ~ScalaSearch() {}

  void clear() {}

  /**
   * Collect communication meta info, bandwidth, comm number.
   */
  void report_comm_info(float time) {
    metrics.report_comm_info(rdma_comm, time);
  }

  void run_rdma();

  void end_rdma();

  void set_ef(size_t ef) { ef_ = ef; }

  void printQueryBatchInfo(float time) {
    metrics.printQueryBatchInfo(rdma_comm, ef_, time);
  }

  void printQueryBatchInfoSave(float time, std::ofstream &out) {
    metrics.printQueryBatchInfoSave(rdma_comm, ef_, time, out);
  }

  std::priority_queue<std::pair<dist_t, labeltype>> searchKnn(
      const void *query_data, size_t k,
      BaseFilterFunctor *isIdAllowed = nullptr) {
    return {};
  }

  void sendTerm(
      size_t correct, size_t total, size_t query_load,
      double cumu_lat_us = 0.0);

  bool check_term();

  void initTerm();

  void initialize_batch(
      const void *query_data, uint32_t vec_size, uint32_t query_num);

  // TODO: can use fusion to reduce unused chunk.
  void do_task(ComputeTask<dist_t> &task);

  void work_steal();

  // Per-query coroutine; all suspension points live in search/traversal.h.
  Corobj<bool> run_query(size_t query_id);

  // Count a finished query once. A second completion used to delete the
  // same query state twice and corrupt the heap on the next allocation.
  void take_query_result(
      TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret,
      uint32_t qid);

  void process_events(
      TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret);

  // Runtime: issue local queries, drain them, then keep serving peers.
  std::vector<QueryState<dist_t> *> submit_query(size_t qid);

  std::vector<QueryState<dist_t> *> drain_queries();

  void serve_until_batch_complete(uint32_t query_num);

 private:
  // Algorithm stages, in execution order.
  void search_top_index(size_t query_id);

  void select_core_machines(size_t query_id);

  bool dispatch_subqueries(size_t query_id);

  int select_candidate_batch(
      size_t query_id, std::pair<dist_t, uint32_t> *group_nodes);

  void expand_candidate_batch(
      size_t query_id, TaskManager<dist_t> &tman,
      std::pair<dist_t, uint32_t> *group_nodes, int gsz);

  void merge_completed_work(size_t query_id);
  void process_local_neighbor_tasks(size_t query_id);
  void process_shared_neighbor_tasks(size_t query_id);
  void process_inline_neighbor_tasks(size_t query_id);
  void process_remote_reads(size_t query_id);
  void process_remote_results(size_t query_id);

  bool advance_termination(size_t query_id);

  void assemble_result(size_t query_id);

  void process_query_messages(
      TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret);

  void process_node_tasks(
      TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret);

  void process_node_results(
      TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret);

  void process_compute_tasks(
      TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret);

  void process_read_completions(
      TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret);

  void process_result_messages(
      TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret);

  void resume_subqueries(
      TaskManager<dist_t> &tman, std::vector<QueryState<dist_t> *> &ret);
};

}  // namespace hnswlib

#include "anns/search/candidate_processing.h"
#include "anns/search/environment.h"
#include "anns/search/event_handlers.h"
#include "anns/search/runtime.h"
#include "anns/search/seed_search.h"
#include "anns/search/termination.h"
#include "anns/search/traversal.h"
