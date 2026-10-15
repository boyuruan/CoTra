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

#include "anns_param.h"
#include "coromem/include/backend.h"
#include "coromem/include/galois/loops.h"
#include "coromem/include/runtime/range.h"
#include "hash_table.h"
#include "index/graph_index.h"
#include "query_msg.h"
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

  std::string evaluation_save_path;
  std::ofstream evaluation_save_file;

  DISTFUNC<dist_t> fstdistfunc_;
  void *dist_func_param_{nullptr};

  AnnsParameter anns_param;
  GraphIndex &graph_index;

  BalanceQueue<ComputeTask<dist_t>> ba_task_queue;

  bool should_term{false};
  bool has_send_term{false};
  QueryMsg<dist_t> **global_query;
  PerThreadStorage<Profiler> all_profiler;

  ScalaScheduler scheduler;

  bool haveInit = false;
  std::atomic<cycles_t> basetime{0};

  RdmaParameter rdma_param;
  RdmaCommunication rdma_comm;

  typedef QueryMsg<dist_t>::CompareByFirst CompareByFirst;

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
    evaluation_save_path = path;
    if (path != "") {
      evaluation_save_file.open(evaluation_save_path, std::ios::out);
      if (evaluation_save_file.is_open()) {
        printf("Open evaluation save file %s\n", evaluation_save_path.c_str());
      } else {
        printf(
            "Open evaluation save file %s failed\n",
            evaluation_save_path.c_str());
      }
    }
  }

  std::ofstream &get_evaluation_save_file() { return evaluation_save_file; }

  // for all app type
  ScalaSearch(
      AnnsParameter anns_param_, RdmaParameter rdma_param_,
      SpaceInterface<dist_t> *space_interface,
      GraphIndex &graph_index_)
      : anns_param(anns_param_),
        rdma_param(rdma_param_),
        graph_index(graph_index_) {
    data_size_ = space_interface->get_data_size();
    data_label_size = data_size_ + sizeof(size_t);
    fstdistfunc_ = space_interface->get_dist_func();
    dist_func_param_ = space_interface->get_dist_func_param();
    if(data_size_ != graph_index.index_param.vec_size){
      std::cerr<<"Error: Mismatch vector data size. \n";
      abort();
    }
    
    global_query =
        (QueryMsg<dist_t> **)new QueryMsg<dist_t> *[anns_param.qsize]();
    printf("ScalaGraph init ...\n");
    graph_index.scala_graph->load_index();
    if (!anns_param.pq_prefix.empty()) init_pq_gate();
    run_rdma();
    printf("Init over ...\n");
  }

  PQCodes pq_codes;

  void init_pq_gate() {
    ScalaGraphIndex *g = graph_index.scala_graph;
    pq_codes.load(
        anns_param.pq_prefix, anns_param.data_type_str,
        g->kmeans_pos_label_map, g->all_vec_num);
    if (pq_codes.dim != anns_param.vecdim) {
      fprintf(
          stderr, "PQ dim %u does not match data dim %zu\n", pq_codes.dim,
          anns_param.vecdim);
      abort();
    }
    // A local vector should be far closer to its own code than to another
    // vector's. If not, the codes are not in this graph's pos order.
    std::mt19937 rng(1);
    double own = 0, other = 0;
    uint32_t n = std::min<uint32_t>(1000, g->local_part_vec_num);
    for (uint32_t i = 0; i < n; i++) {
      uint32_t local = rng() % g->local_part_vec_num;
      const char *vec = g->vector_ptr + (size_t)local * data_label_size;
      own += pq_codes.dist(vec, g->partition_ofs + local);
      other += pq_codes.dist(vec, rng() % g->all_vec_num);
    }
    printf(
        "PQ self-check over %u local vectors: own code %.1f, random code "
        "%.1f\n",
        n, own / n, other / n);
    if (own >= 0.5 * other) {
      fprintf(stderr, "PQ codes do not match the graph's vector order\n");
      abort();
    }
    g->pq = &pq_codes;
    g->pq_shadow = anns_param.pq_tau < 0;
    g->pq_scale = 1.0f + std::max(0.0f, anns_param.pq_tau);
    printf(
        "PQ gate on inline remote neighbors: tau %.3f%s\n", anns_param.pq_tau,
        g->pq_shadow ? " (shadow: count only)" : "");
  }

  ~ScalaSearch() {}

  void clear() {}

  /**
   * Collect communication meta info, bandwidth, comm number.
   */
  void report_comm_info(float time) {
    uint64_t all_read_size = 0;
    uint64_t all_write_size = 0;
    uint64_t all_read_cnt = 0;
    uint64_t all_write_cnt = 0;
    uint64_t type_size[CLEAR + 1] = {0}, type_cnt[CLEAR + 1] = {0};
    uint64_t deliv[CH_NUM] = {0}, dup[CH_NUM] = {0}, pass[CH_NUM] = {0};
    uint64_t gated[CH_NUM] = {0};
    // [purpose][cnt, head, top, cand, result]
    uint64_t query[2][5] = {{0}};
    // cnt, head, cand, target, token, micro entries, large entries
    uint64_t sync[7] = {0};
    // expand, node task, exact dist, pq dist
    uint64_t work[4] = {0};

    for (uint32_t t = 0; t < getActiveThreads(); ++t) {
      auto *ctx = rdma_comm.rdma_ctx.getRemote(t);
      all_read_size += ctx->read_size;
      all_write_size += ctx->write_size;
      all_read_cnt += ctx->read_cnt;
      all_write_cnt += ctx->write_cnt;
      for (int c = 0; c <= CLEAR; c++) {
        type_size[c] += ctx->type_write_size[c];
        type_cnt[c] += ctx->type_write_cnt[c];
      }
      for (int c = 0; c < CH_NUM; c++) {
        deliv[c] += ctx->chan_deliv[c];
        dup[c] += ctx->chan_dup[c];
        pass[c] += ctx->chan_pass[c];
        gated[c] += ctx->chan_gated[c];
      }
      for (int p = 0; p < 2; p++) {
        query[p][0] += ctx->query_cnt[p];
        query[p][1] += ctx->query_head[p];
        query[p][2] += ctx->query_top[p];
        query[p][3] += ctx->query_cand[p];
        query[p][4] += ctx->query_result[p];
      }
      sync[0] += ctx->sync_cnt;
      sync[1] += ctx->sync_head;
      sync[2] += ctx->sync_cand;
      sync[3] += ctx->sync_target;
      sync[4] += ctx->sync_token;
      sync[5] += ctx->sync_micro;
      sync[6] += ctx->sync_large;
      work[0] += ctx->work_expand;
      work[1] += ctx->work_node_task;
      work[2] += ctx->work_dist;
      work[3] += ctx->work_pq_dist;
    }
    float read_sz_ps = 1000000.0 * (all_read_size >> 20) / time;
    float read_num_ps = 1000000.0 * all_read_cnt / time;
    float write_sz_ps = 1000000.0 * (all_write_size >> 20) / time;
    float write_num_ps = 1000000.0 * all_write_cnt / time;
    printf("-------------- Overall COMM Info --------------\n");
    printf("Read: %.2f MB/s, %.2f num/s\n", read_sz_ps, read_num_ps);
    printf("Write: %.2f MB/s, %.2f num/s\n", write_sz_ps, write_num_ps);
    printf(
        "ReadBytes: %llu ReadCnt: %llu WriteBytes: %llu WriteCnt: %llu\n",
        (unsigned long long)all_read_size, (unsigned long long)all_read_cnt,
        (unsigned long long)all_write_size, (unsigned long long)all_write_cnt);
    static const char *type_name[CLEAR + 1] = {
        "PART_INFO",     "BUILD_SYNC",  "DISPATCH_META", "DISPATCH_ID",
        "DISPATCH_NGH",  "SCALA_META",  "SCALA_NGH",     "TASK",
        "RESULT",        "NODE_RESULT", "NODE_SYNC",     "COMPUTE",
        "QUERY",         "CORE_INFO",   "SCHEDULE",      "SYNC",
        "RELEASE",       "TERMINATION", "CLEAR"};
    for (int c = 0; c <= CLEAR; c++) {
      if (type_cnt[c] == 0) continue;
      printf(
          "WriteType: %s Bytes: %llu Cnt: %llu\n", type_name[c],
          (unsigned long long)type_size[c], (unsigned long long)type_cnt[c]);
    }
    static const char *chan_name[CH_NUM] = {
        "READ", "MICRO", "RESULT", "NODE_RESULT"};
    for (int c = 0; c < CH_NUM; c++) {
      printf(
          "Chan: %s Deliv: %llu Dup: %llu Pass: %llu Gated: %llu\n",
          chan_name[c], (unsigned long long)deliv[c],
          (unsigned long long)dup[c], (unsigned long long)pass[c],
          (unsigned long long)gated[c]);
    }
    // QUERY bytes by purpose and section, SYNC bytes by section, and the
    // work this machine did. Byte sections of a type sum to its WriteType.
    static const char *query_name[2] = {"FORK", "MIGRATE"};
    for (int p = 0; p < 2; p++) {
      printf(
          "Query: %s Cnt: %llu Head: %llu Top: %llu Cand: %llu Result: %llu\n",
          query_name[p], (unsigned long long)query[p][0],
          (unsigned long long)query[p][1], (unsigned long long)query[p][2],
          (unsigned long long)query[p][3], (unsigned long long)query[p][4]);
    }
    printf(
        "Sync: Cnt: %llu Head: %llu Cand: %llu Target: %llu Token: %llu "
        "Micro: %llu Large: %llu\n",
        (unsigned long long)sync[0], (unsigned long long)sync[1],
        (unsigned long long)sync[2], (unsigned long long)sync[3],
        (unsigned long long)sync[4], (unsigned long long)sync[5],
        (unsigned long long)sync[6]);
    printf(
        "Work: Expand: %llu NodeTask: %llu Dist: %llu PqDist: %llu\n",
        (unsigned long long)work[0], (unsigned long long)work[1],
        (unsigned long long)work[2], (unsigned long long)work[3]);
    printf("--------------------  End  --------------------\n");
  }

  void run_rdma() {
    partition_ptr = graph_index.get_partition_ptr();
    partition_size = graph_index.get_partition_size();
    rdma_comm.com_init(partition_ptr, partition_size, rdma_param);
    rdma_comm.com_read_init();
    rdma_comm.com_write_init();
  }

  void end_rdma() {
    if (rdma_comm.ctx_close_connection()) {
      fprintf(stderr, "Failed to close connection between server and client\n");
      return;
    }
  }

  void set_ef(size_t ef) { ef_ = ef; }

  void printQueryBatchInfo(float time) {
    float recall = 1.0f * rdma_comm.correct / rdma_comm.total;
    float avg_lat = rdma_comm.query_lat_sum / rdma_comm.query_load;
    size_t all_comp_cnt = rdma_comm.all_computation_cnt;
    float qps = 1000000.0 * rdma_comm.query_load / time;
#ifdef PROF_COMPUTATION
    printf(
        "%d \t %.5f \t %.2f \t %.3f \t %llu \n", ef_, recall, avg_lat,
        qps, all_comp_cnt);
#else
    printf(
        "%d \t %.5f \t %.2f \t %.3f \t \n", ef_, recall, avg_lat,
        qps);
#endif
  }

  void printQueryBatchInfoSave(float time, std::ofstream &out) {
    float recall = 1.0f * rdma_comm.correct / rdma_comm.total;
    float avg_lat = rdma_comm.query_lat_sum / rdma_comm.query_load;
    float qps = 1000000.0 * rdma_comm.query_load / time;
    printf("-------------- Overall info --------------\n");
    printf(
        "correct: %llu total: %llu query load: %llu\n", rdma_comm.correct,
        rdma_comm.total, rdma_comm.query_load);
    printf(
        "%d \t %.5f \t %.2f \t %.3f \t \n", ef_, recall, avg_lat,
        qps);
    printf("------------------  End ------------------\n");

    out << ef_ << "," << recall << "," << avg_lat << "," << qps
        << std::endl;
  }

  std::priority_queue<std::pair<dist_t, labeltype>> searchKnn(
      const void *query_data, size_t k,
      BaseFilterFunctor *isIdAllowed = nullptr) {
    return {};
  }

  void sendTerm(size_t correct, size_t total, size_t query_load, double cumu_lat_us = 0.0) {
#ifdef PROF_COMPUTATION
    rdma_comm.send_term(rdma_param.machine_id, correct, total, 
      query_load, cumu_lat_us, computation_cnt);
    computation_cnt = 0;
#else 
    rdma_comm.send_term(rdma_param.machine_id, correct, total, 
      query_load, cumu_lat_us, 0);
#endif
    return;
  }

  bool check_term() {
    rdma_comm.poll_send();
    rdma_comm.poll_recv();
    if (rdma_comm.check_term()) {
      rdma_comm.reset_sendrecv();
      return true;
    }

    return false;
  }

  void initTerm() { rdma_comm.reset_term(); }


  void scala_search_init(
      const void *query_data, uint32_t vec_size, uint32_t query_num) {
    // printf("scala search init\n");
#ifdef PROF_DEGREE
    memset(local_distri, 0, sizeof(local_distri));
    memset(remote_distri, 0, sizeof(remote_distri));
    local_cnt = local_deg = remote_cnt = remote_deg = 0;
#endif

    for (uint32_t q = 0; q < query_num; q++) {
      // Previous ef's objects stay until here. Recall accounting does not
      // delete them: a second completion would free the same pointer twice.
      delete global_query[q];
      global_query[q] = new QueryMsg<dist_t>(
          (char *)(query_data + (uint64_t)q * vec_size), vec_size, q, ef_,
          rdma_param.machine_id);
    }
    on_each([&](uint64 tid, uint64 total) {
#ifdef PROFILER
      auto *local_prof = all_profiler.getLocal();
      Profiler &m_profiler = *local_prof;
      m_profiler.clear();
#endif
      // init comm size count.
      auto *ctx = rdma_comm.rdma_ctx.getLocal();
#ifdef COMM_PROFILE
      ctx->clear_size_cnt();
#endif
      auto *tman = task_ma.getLocal();
      tman->clear();

      delete[] tman->global_query;
      tman->global_query =
          (QueryMsg<dist_t> **)new QueryMsg<dist_t> *[anns_param.qsize];
      for (uint32_t q = 0; q < query_num; q++) {
        // Init query.
        tman->global_query[q] = global_query[q];
      }

      tman->ba_task_queue = &ba_task_queue;
      if (graph_index.scala_graph != nullptr) {
        tman->machine_of = [this](uint32_t id) {
          return graph_index.scala_graph->get_machine_id(id);
        };
      }
    });
    // printf("scala search init over\n");
  }

  // TODO: can use fusion to reduce unused chunk.
  void do_task(ComputeTask<dist_t> &task) {
#ifdef DEBUG
    printf("do task %u\n", task.qid);
#endif
#ifdef PROFILER
    Profiler &m_profiler = *all_profiler.getLocal();
    m_profiler.start("g_do_task");
#endif
    _mm_prefetch(global_query[task.qid]->vector, _MM_HINT_T0);
    // auto &task = *global_query[query_id]->cur_task;
    char *vec_ptr = graph_index.scala_graph->vector_ptr;
    uint32_t part_ofs =
        graph_index.scala_graph->scala_graph_get_partition_ofs();
    // printf(
    //     "local task size: %u\n",
    //     global_query[query_id]->local_task.size());
    // printf("%llu %u \n", task.offset, task.deg);
    uint32_t *ngh_arr =
        graph_index.scala_graph->scala_graph_v3_get_ngh_ptr(task.offset);
    _mm_prefetch((char *)ngh_arr, _MM_HINT_T0);
    _mm_prefetch(vec_ptr + ((*(ngh_arr)) - part_ofs) * data_label_size, _MM_HINT_T0);
    _mm_prefetch((char *)(ngh_arr + 1), _MM_HINT_T0);
    worklists::Chunk<std::pair<dist_t, uint32_t>, MAX_GRAPH_DEG> chunk(
        task.is_count);
#ifdef COMM_PROFILE
    RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif
    for (uint32_t d = 0; d < task.deg; d++) {
      _mm_prefetch(
          vec_ptr + ((*(ngh_arr + d + 1)) - part_ofs) * data_label_size, _MM_HINT_T0);
      // printf("%u\n", *(ngh_arr + d));
      // TODO: may have problem with concurrent
      if (!global_query[task.qid]->visit_hash.CheckAndSet(*(ngh_arr + d))) {
#ifdef PROF_COMPUTATION
        computation_cnt ++;
#endif
#ifdef COMM_PROFILE
        comm_ctx->work_dist++;
#endif
        char *vec_data = vec_ptr + ((*(ngh_arr + d)) - part_ofs) * data_label_size;
        dist_t dist = fstdistfunc_(
            global_query[task.qid]->vector, vec_data, dist_func_param_);
        chunk.emplace_back(dist, *(ngh_arr + d));
      }
    }
    global_query[task.qid]->con_task_queue.push(chunk);
#ifdef PROFILER
    m_profiler.end("g_do_task");
#endif
#ifdef DEBUG
    printf("do task %u over\n", task.qid);
#endif
    return;
  }

  void work_steal() {
#ifdef PROFILER
    Profiler &m_profiler = *all_profiler.getLocal();
    m_profiler.start("g_steal");
#endif
    // Use work stealing across threads.
    auto task = ba_task_queue.pop();
    if (task.has_value()) {
      do_task(task.value());
    }
#ifdef PROFILER
    m_profiler.end("g_steal");
#endif
  }

  /**
   * Add cached topindex search.
   */
  Corobj<bool> coro_sub_query_v5(size_t query_id) {
    TaskManager<dist_t> &tman = *task_ma.getLocal();
#ifdef PROFILER
    Profiler &m_profiler = *all_profiler.getLocal();
#endif

    auto top_proc_candidate = [&](dist_t dist, uint32_t candidate_id) {
      bool flag_consider_candidate =
          global_query[query_id]->pre_top_cand.size() <
              global_query[query_id]->ef ||
          global_query[query_id]->lowerBound > dist;

      if (flag_consider_candidate) {
        global_query[query_id]->pre_cand_set.emplace(-dist, candidate_id);
        global_query[query_id]->pre_top_cand.emplace(dist, candidate_id);

        // if (graph_index.top_index->is_last_level(candidate_id)) {
        //   // In the 5-th level hop, put into final canidate set (prestage).
        //   uint32_t gt_id =
        //   graph_index.top_index->get_global_id(candidate_id);
        //   global_query[query_id]->candidate_set.emplace(-dist, gt_id);
        // }

        while (global_query[query_id]->pre_top_cand.size() >
               global_query[query_id]->ef) {
          global_query[query_id]->pre_top_cand.pop();
        }

        if (!global_query[query_id]->pre_top_cand.empty())
          global_query[query_id]->lowerBound =
              global_query[query_id]->pre_top_cand.top().first;
      }
    };

    auto post_proc_candidate = [&](dist_t dist, uint32_t candidate_id) -> bool {
      bool flag_consider_candidate =
          global_query[query_id]->top_candidates.size() <
              global_query[query_id]->ef ||
          global_query[query_id]->lowerBound > dist;

      if (flag_consider_candidate) {
        global_query[query_id]->candidate_set.emplace(-dist, candidate_id);
        global_query[query_id]->top_candidates.emplace(dist, candidate_id);

        // update sync msg.
        global_query[query_id]->sync_msg.local_add_cand.emplace_back(
            dist, candidate_id);

        // do process, update local term
        global_query[query_id]->proc_is_black |= 1;

        while (global_query[query_id]->top_candidates.size() >
               global_query[query_id]->ef) {
          tableint id = global_query[query_id]->top_candidates.top().second;
          global_query[query_id]->top_candidates.pop();
        }

        if (!global_query[query_id]->top_candidates.empty())
          global_query[query_id]->lowerBound =
              global_query[query_id]->top_candidates.top().first;
      }
      return flag_consider_candidate;
    };

#ifdef COMM_PROFILE
    RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif

    std::pair<dist_t, uint32_t> group_nodes[CAND_GROUP_SIZE];
    if (global_query[query_id]->is_new) {
#ifdef DEBUG
      printf("start new query %u, Pre STAGE\n", query_id);
#endif
      global_query[query_id]->state = PRE_STAGE;

#ifdef PROFILER
      global_query[query_id]->profiler.start("q_pre-stage");
      global_query[query_id]->profiler.start("q_over_all");
#endif
      // in scala v3, cur_query_num record the number of query in pre stage.
      tman.cur_query_num++;
      // tableint currObj = graph_index.scala_graph->enter_p;
      uint32_t enterpoint = 0;
      // assume enter point is cached.
      // char *ep_data = graph_index.scala_graph->enter_p_ptr;
      char *ep_data = graph_index.top_index->get_vector(enterpoint);

      // Traverse on base index. (Use one-side migrate)
      dist_t dist = fstdistfunc_(
          global_query[query_id]->vector, ep_data, dist_func_param_);
#ifdef COMM_PROFILE
      comm_ctx->work_dist++;
#endif
      global_query[query_id]->lowerBound = dist;
      global_query[query_id]->pre_top_cand.emplace(dist, enterpoint);
      global_query[query_id]->pre_cand_set.emplace(-dist, enterpoint);

      uint32_t gt_id = graph_index.top_index->get_global_id(enterpoint);
      global_query[query_id]->visit_hash.CheckAndSet(gt_id);
      // global_query[query_id]->current_node_id = currObj;
      global_query[query_id]->ef = std::max((uint32_t)ef_, anns_param.res_knn);
      // global_query[query_id]->max_path_id = rdma_param.machine_id;
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

          // TODO: can prefetch here.
          uint32_t gt_id = graph_index.top_index->get_global_id(ngh_arr[d]);
          if (!global_query[query_id]->visit_hash.CheckAndSet(gt_id)) {
#ifdef PROF_COMPUTATION
            computation_cnt ++;
#endif
#ifdef COMM_PROFILE
            comm_ctx->work_dist++;
#endif
            char *vec_data = graph_index.top_index->get_vector(ngh_arr[d]);
            dist_t dist = fstdistfunc_(
                global_query[query_id]->vector, vec_data, dist_func_param_);
            top_proc_candidate(dist, ngh_arr[d]);
          }
        }
        // printf("all_result_recved__\n");
      }
      // tman.cur_query_num--;

#ifdef DEBUG
      printf("Start to dispatch sub-query of %u\n", query_id);
#endif
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
        global_query[query_id]->filterred[mid].emplace_back(
          -cand_top.first, gt_id);
        // if (graph_index.top_index->is_last_level(cand_top.second)) {
          // global_query[query_id]->filterred[mid].emplace_back(
          //     -cand_top.first, gt_id);
          // global_query[query_id]->filter_num[mid]++;
        // }
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
            global_query[query_id]->filterred[mx_mid].emplace_back(cand);
          }
        }
      }

      // Dispatch sub-query: fork query to core machine.
      bool local_is_core = false;
      for (uint32_t &m : global_query[query_id]->core_machine) {
        // printf("%u ", m);
        // rdma_comm.migrate_query(m, global_query[query_id]);
        if (m == rdma_param.machine_id) {
          // local machine.
          local_is_core = true;
          for (auto &cand : global_query[query_id]->filterred[m]) {
            global_query[query_id]->candidate_set.emplace(cand);
          }
        } else {
          rdma_comm.fork_query(m, global_query[query_id]);
        }
      }
      global_query[query_id]->visit_hash.clear();

      // Send core machine info to leader machine scheduler.
      // printf("send core machine info to leader machine\n");
      // rdma_comm.send_core_machine_info(tman, global_query[query_id]);

      // printf("\n");
#ifdef PROFILER
      global_query[query_id]->profiler.end("q_pre-stage");
#endif

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

      // tman.post_async = tman.post_task = 0;
      // tman.rpost_async = tman.rpost_task = 0;

      while (!global_query[query_id]->global_term) {
        for (;;) {
#ifdef PROFILER
          m_profiler.start("post_task");
#endif
          int gsz = 0;
          for (gsz = 0; gsz < CAND_GROUP_SIZE &&
                        !global_query[query_id]->candidate_set.empty();
               gsz++) {
            group_nodes[gsz] = global_query[query_id]->candidate_set.top();
            dist_t candidate_dist = -group_nodes[gsz].first;
            if (candidate_dist > global_query[query_id]->lowerBound) {
              break;
            }
            global_query[query_id]->candidate_set.pop();
            // do not release here, since may in top candidates queue.
          }
          // post micro task to remote machine.
          if (gsz > 0) {
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
                post_cnt += graph_index.scala_graph
                                ->scala_graph_v3_get_subquery_task_lb(
                                    ngh_id, query_id, tman, ba_task_queue,
                                    gated, pq_dist);
              }
            }
#ifdef DEBUG
            std::cout<<"get remote task over, post\n";
#endif
            // Post micro task to non-core machine.
            rdma_comm.post_remote_task(
                tman, query_id, data_size_ + sizeof(uint64_t), &global_query[query_id]->profiler);

            // sync msg & task to core machine.
            // untill end sync now.
            global_query[query_id]->sync_step++;
            if (global_query[query_id]->sync_step >= SYNC_BAR) {
              global_query[query_id]->sync_msg.lower_bound =
                  global_query[query_id]->lowerBound;
              rdma_comm.post_subquery_sync(global_query[query_id]);

              global_query[query_id]->sync_msg.clear();
              global_query[query_id]->sync_step = 0;
            }
#ifdef DEBUG
            std::cout<<"remote task post over\n";
#endif

            // put subquery to next iteration global subquery queue.
            if (global_query[query_id]->state == POST_STAGE) {
              tman.next_subquery_queue.push_back(query_id);
              // printf("push q%u to next sub\n", query_id);
            }
#ifdef PROFILER
            m_profiler.end("post_task");
#endif
          } else {
            all_result_recved = (recv_cnt == post_cnt) && (nocore_post == nocore_recv);
            // printf(
            //     "q%d post_cnt %d recv_cnt %d\n", query_id, post_cnt,
            //     recv_cnt);

#ifdef PROFILER
            m_profiler.end("post_task");
#endif
            if (!all_result_recved) {
              // global_query[query_id]->state = PAUSE;
              tman.next_subquery_queue.push_back(query_id);
            } else if (
                all_result_recved &&
                global_query[query_id]->state == POST_STAGE) {
              // printf("try to terminate\n");
              break;
            }
            // else all_result_recved &&
            // global_query[query_id]->state == PAUSE, wait for global term.
          }

          // if (global_query[query_id]->state == END) {
          //   printf("Error: query %u state is END\n", query_id);
          //   abort();
          // }

          /*
            suspend to proc recved remote tasks .
            */
          // printf("post suspend with p %u r %u\n", post_cnt, recv_cnt);
          co_yield false;
          // printf("q%u search RESUME\n", query_id);
#ifdef PROFILER
          m_profiler.start("post_stage");
#endif
// #ifdef DEBUG
//           printf("q%u continue\n", query_id);
// #endif

          _mm_prefetch(global_query[query_id]->vector, _MM_HINT_T0);
          // auto &task = *global_query[query_id]->cur_task;
          char *vec_ptr = graph_index.scala_graph->vector_ptr;
          uint32_t part_ofs =
              graph_index.scala_graph->scala_graph_get_partition_ofs();
          // printf(
          //     "local task size: %u\n",
          //     global_query[query_id]->local_task.size());
          if(global_query[query_id]->local_task.size()){
            #ifdef DEBUG
              printf("proc qlocal task\n");
            #endif 
            for (ComputeTask<dist_t> &task : global_query[query_id]->local_task) {
              // printf("%llu %u \n", task.offset, task.deg);
              uint32_t *ngh_arr =
                  graph_index.scala_graph->scala_graph_v3_get_ngh_ptr(
                      task.offset);
              _mm_prefetch((char *)ngh_arr, _MM_HINT_T0);
              _mm_prefetch(
                  vec_ptr + ((*(ngh_arr)) - part_ofs) * data_label_size, _MM_HINT_T0);
              _mm_prefetch((char *)(ngh_arr + 1), _MM_HINT_T0);
              for (uint32_t d = 0; d < task.deg; d++) {
                _mm_prefetch(
                    vec_ptr + ((*(ngh_arr + d + 1)) - part_ofs) * data_label_size,
                    _MM_HINT_T0);
                // printf("%u\n", *(ngh_arr + d));
                if (!global_query[query_id]->visit_hash.CheckAndSet(
                        *(ngh_arr + d))) {
#ifdef PROF_COMPUTATION
                  computation_cnt ++;
#endif
#ifdef COMM_PROFILE
                  comm_ctx->work_dist++;
#endif
                  char *vec_data =
                      vec_ptr + ((*(ngh_arr + d)) - part_ofs) * data_label_size;
                  dist_t dist = fstdistfunc_(
                      global_query[query_id]->vector, vec_data, dist_func_param_);
                  post_proc_candidate(dist, *(ngh_arr + d));
                }
              }
            }
            global_query[query_id]->local_task.clear();
            #ifdef DEBUG
              printf("proc qlocal task over\n");
            #endif 
          }
          

          // Proc balance task queue. (Shared local task)
          // printf("]]]pop ba_task_queue\n");

          for (;;) {
            auto task = ba_task_queue.pop(false);
            if (!task.has_value()) {
              break;
            }
            // Use work stealing across threads.
            do_task(task.value());
          }

          for (;;) {
            auto chunk = global_query[query_id]->con_task_queue.pop();
            if (!chunk.has_value()) {
              break;
            }
#ifdef DEBUG
            printf("chunk size: %u\n", chunk->size());
#endif
            recv_cnt += chunk.value().isCount();
            for (int i = 0; i < chunk.value().size(); i++) {
              auto &ele = chunk.value().getAt(i);
              post_proc_candidate(ele.first, ele.second);
            }
          }
          // printf("ccnt: %u\n", ccnt);
          // printf("]]]pop con_task_queue over\n");

          // printf(
          //     "local micro task size: %u\n",
          //     global_query[query_id]->local_micro_task.size());
          // local_micro_task: recved ngh from other machine.
          if (global_query[query_id]->local_micro_task.size()) {
            #ifdef DEBUG
              printf("proc qlocal micro task\n");
            #endif 
            // printf("proc local micro task\n");
            for (uint32_t i = 0;
                 i < global_query[query_id]->local_micro_task.size(); i++) {
              uint32_t &vid = global_query[query_id]->local_micro_task[i];
              _mm_prefetch(
                  vec_ptr + (global_query[query_id]->local_micro_task[i + 1] -
                             part_ofs) *
                                data_label_size,
                  _MM_HINT_T0);
#ifdef COMM_PROFILE
              comm_ctx->chan_deliv[CH_MICRO]++;
#endif
              if (!global_query[query_id]->visit_hash.CheckAndSet(vid)) {
#ifdef PROF_COMPUTATION
                computation_cnt ++;
#endif
#ifdef COMM_PROFILE
                comm_ctx->work_dist++;
#endif
                char *vec_data = vec_ptr + (vid - part_ofs) * data_label_size;
                dist_t dist = fstdistfunc_(
                    global_query[query_id]->vector, vec_data, dist_func_param_);
                bool took = post_proc_candidate(dist, vid);
#ifdef COMM_PROFILE
                comm_ctx->chan_pass[CH_MICRO] += took;
#endif
              } else {
#ifdef COMM_PROFILE
                comm_ctx->chan_dup[CH_MICRO]++;
#endif
              }
            }
            global_query[query_id]->local_micro_task.clear();
            #ifdef DEBUG
              printf("proc qlocal micro task over\n");
            #endif 
          }

          //  post stage async read queue.
          // printf(
          //     "recv_async_read_queue size: %u\n",
          //     global_query[query_id]->recv_async_read_queue.size());
          if(global_query[query_id]->recv_async_read_queue.size()){
            #ifdef DEBUG
              printf("proc async_read\n");
            #endif 
            for (BufferCache &r : global_query[query_id]->recv_async_read_queue) {
              uint32_t ngh_id = r.vector_id;
              char *vec_ptr = r.buffer_ptr;
              // printf("proc ngh_id: %u bufid: %u\n", ngh_id, r.buffer_id);
              _mm_prefetch(vec_ptr, _MM_HINT_T0);
#ifdef COMM_PROFILE
              comm_ctx->chan_deliv[CH_READ]++;
#endif
              if (!global_query[query_id]->visit_hash.CheckAndSet(ngh_id)) {
#ifdef PROF_COMPUTATION
                computation_cnt ++;
#endif
#ifdef COMM_PROFILE
                comm_ctx->work_dist++;
#endif
                dist_t dist = fstdistfunc_(
                    global_query[query_id]->vector, vec_ptr, dist_func_param_);
                bool took = post_proc_candidate(dist, ngh_id);
#ifdef COMM_PROFILE
                comm_ctx->chan_pass[CH_READ] += took;
#endif
              } else {
#ifdef COMM_PROFILE
                comm_ctx->chan_dup[CH_READ]++;
#endif
              }
              rdma_comm.release_cache(r.buffer_id);
              // free(r);
              recv_cnt++;
              // tman.rpost_async++;
              // printf(
              //     "q%u recv_cnt after async %u post_cnt %u tman t%u a%u rt%u "
              //     "ra%u\n",
              //     query_id, recv_cnt, post_cnt, tman.post_task,
              //     tman.post_async, tman.rpost_task, tman.rpost_async);
            }
            // global_query[query_id]->has_cur_async = false;
            global_query[query_id]->recv_async_read_queue.clear();
            #ifdef DEBUG
              printf("proc async_read over\n");
            #endif 
          }
          

          // if (global_query[query_id]->has_cur_res) {
          // printf(
          //     "local_res size: %u\n",
          //     global_query[query_id]->local_res.size());
          if (global_query[query_id]->local_res.size()) {
            #ifdef DEBUG
              printf("proc qlocal_res\n");
            #endif 
            // printf("proc q%u local res\n", query_id);
            for (ResultMsg<dist_t> &result :
                 global_query[query_id]->local_res) {
              // auto &result = *global_query[query_id]->cur_res;
              for (std::pair<dist_t, uint32_t> &r : result.res) {
                dist_t dist = r.first;
                uint32_t candidate_id = r.second;
                bool took = post_proc_candidate(dist, candidate_id);
#ifdef COMM_PROFILE
                int ch = result.from_node ? CH_NODE_RESULT : CH_RESULT;
                comm_ctx->chan_deliv[ch]++;
                comm_ctx->chan_pass[ch] += took;
#endif
              }
              // count the res from core and nocore result.
              recv_cnt += result.core_cnt;
              nocore_recv += result.nocore_cnt;
            }
            global_query[query_id]->local_res.clear();
            #ifdef DEBUG
              printf("proc qlocal_res over\n");
            #endif 
          }

#ifdef PROFILER
          m_profiler.end("post_stage");
#endif
        }

        // local termination, prop token
        // printf("q%u start term\n", query_id);
        if (global_query[query_id]->core_machine.size() > 1) {
          if (global_query[query_id]->has_token) {
            // if is leader
            // printf("q%u has token\n", query_id);
            if (global_query[query_id]->leader_machine ==
                rdma_param.machine_id) {
              // printf(
              //     "q%u m%u is leader machine\n", query_id,
              //     rdma_param.machine_id);
              bool failed = global_query[query_id]->token_is_black ||
                            global_query[query_id]->proc_is_black;
              global_query[query_id]->token_is_black =
                  global_query[query_id]->proc_is_black = false;
              if (global_query[query_id]->last_is_white && !failed) {
                // This was the second success
                // printf("The second term success, q%u END\n", query_id);
                global_query[query_id]->global_term = true;
                break;
              }
              global_query[query_id]->last_is_white = !failed;
              // printf("The fisrt term success, second term failed\n");
            }
            bool taint = global_query[query_id]->proc_is_black ||
                         global_query[query_id]->token_is_black;
            global_query[query_id]->proc_is_black =
                global_query[query_id]->token_is_black = false;

            // prop token.
            rdma_comm.post_subquery_sync(global_query[query_id], true, taint);
            global_query[query_id]->sync_msg.clear();
            global_query[query_id]->has_token = false;

            // printf("q%u post PROP sync\n", query_id);
            global_query[query_id]->sync_step = 0;
          }
          global_query[query_id]->state = PAUSE;
          // printf("q%u search PAUSE\n", query_id);
        } else {
          // single core machine, just end.
          global_query[query_id]->global_term = true;
          // printf("The first term success, q%u END\n", query_id);
          break;
        }
      }
    }
    // printf("search END\n");
    global_query[query_id]->state = END;
// printf("q%d over\n", query->query_id);
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
      global_query[query_id]->result.push(std::pair<dist_t, labeltype>(
          rez.first,
          graph_index.scala_graph->kmeans_pos_label_map[rez.second]));
      global_query[query_id]->top_candidates.pop();
    }

    // migrate to origin machine
    if (global_query[query_id]->leader_machine == rdma_param.machine_id &&
        rdma_param.machine_id != global_query[query_id]->origin_machine) {
      // printf(
      //     "migrate q%u to origin machine m%u...\n", query_id,
      //     global_query[query_id]->origin_machine);
      rdma_comm.migrate_query(
          global_query[query_id]->origin_machine, global_query[query_id]);
      co_yield false;
    }
#ifdef PROFILER
    global_query[query_id]->profiler.end("q_over_all");
#endif
    co_yield true;
  }

  // Count a finished query once. A second completion used to delete the
  // same QueryMsg twice and corrupt the heap on the next allocation.
  void take_query_result(
      TaskManager<dist_t> &tman, std::vector<QueryMsg<dist_t> *> &ret,
      uint32_t qid) {
    QueryMsg<dist_t> *q = global_query[qid];
    if (q->result_taken) return;
    q->result_taken = true;
    ret.push_back(q);
    tman.cur_query_num--;
  }

  void scala_sub_proc_v5(
      TaskManager<dist_t> &tman, std::vector<QueryMsg<dist_t> *> &ret) {
    auto &task_queue = tman.get_local_task_queue();
    auto &node_queue = tman.get_node_task_queue();
    auto &node_res_queue = tman.get_node_result_queue();
#ifdef COMM_PROFILE
    RdmaContext *comm_ctx = rdma_comm.rdma_ctx.getLocal();
#endif

    auto &result_queue = tman.get_result_queue();
    // migrate query queue, in v3 only used for end query.
    auto &query_queue = tman.get_query_queue();
    auto &async_queue = tman.get_async_task_queue();
    auto &subquery_queue = tman.get_subquery_queue();
#ifdef PROFILER
    Profiler &m_profiler = *all_profiler.getLocal();
#endif

    // Leader machine leader thread, do schduler.
    //     if (anns_param.machine_id == 0 && ThreadPool::getTID() == 0) {
    //       // collect all thread's core_info.
    // #ifdef DEBUG
    //       // printf("collect schedule plan\n");
    // #endif
    //       for (uint32_t t = 1; t < getActiveThreads(); t++) {
    //         auto *tma = task_ma.getRemote(t);
    //         // TODO: change to PaddedLock.
    //         tma->schequery_mutex.lock();
    //         for (auto sche_msg : tma->schedule_query_queue) {
    //           tman.schedule_query_queue.push_back(sche_msg);
    //         }
    //         tma->schedule_query_queue.clear();
    //         tma->schequery_mutex.unlock();
    //       }
    // #ifdef DEBUG
    //     // printf("generate schedule plan\n");
    // #endif
    //     // Generate schedule plan.
    //     scheduler.generate_schedule_plan(tman.schedule_query_queue);
    //     // Send schedule plan to machine.
    //     rdma_comm.send_schedule_plan(scheduler);
    //     // printf("send schedule plan over\n");
    //   }

    // printf("proc subquery\n");
    /**
     * Proc remote task, local task, async read, and results.
     */
    if (query_queue.size()) {
#ifdef PROFILER
      m_profiler.start("g_query_Q");
#endif
      for (auto qid : query_queue) {
        if (global_query[qid]->is_new) {
          // new query
          // printf("recv new sub-query %u\n", qid);
          global_query[qid]->is_new = false;
          if (global_query[qid]->leader_machine == rdma_param.machine_id) {
            global_query[qid]->has_token = 1;
            // printf("q%u leader is me.\n", qid);
          }
          global_query[qid]->coro = coro_sub_query_v5(qid);
        } else {
// end query
// printf("recv end q%u\n", qid);
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
          graph_index.scala_graph->scala_graph_v3_get_nodetask_msg(
              tman, task, res_msg);

          // printf("]]post msg post_cnt +=%d\n", res_msg.post_cnt);

          // ResultMsg<dist_t> res_msg(task.qid);
          // Start to traverse local partition ngh, based on task offset got.
          // printf(
          //     "]]] task offset %llu qid %llu deg %llu machine_id %llu\n",
          //     task.offset, task.qid, task.deg, task.machine_id);
          if(task.offset){
            uint32_t *ngh_arr =
                graph_index.scala_graph->scala_graph_v3_get_ngh_ptr(task.offset);
            char *vec_ptr = graph_index.scala_graph->vector_ptr;
            uint32_t part_ofs =
                graph_index.scala_graph->scala_graph_get_partition_ofs();
            _mm_prefetch(
                vec_ptr + ((*(ngh_arr)) - part_ofs) * data_label_size, _MM_HINT_T0);
            _mm_prefetch(global_query[task.qid]->vector, _MM_HINT_T0);
            _mm_prefetch((char *)(ngh_arr + 1), _MM_HINT_T0);
            for (uint32_t d = 0; d < task.deg; d++) {
              _mm_prefetch(
                  vec_ptr + ((*(ngh_arr + d + 1)) - part_ofs) * data_label_size,
                  _MM_HINT_T0);
              if (!global_query[task.qid]->visit_hash.CheckAndSet(ngh_arr[d])) {
#ifdef PROF_COMPUTATION
                computation_cnt ++;
#endif
#ifdef COMM_PROFILE
                comm_ctx->work_dist++;
#endif
                char *vec_data = vec_ptr + ((*(ngh_arr + d)) - part_ofs) * data_label_size;
                dist_t dist = fstdistfunc_(
                    global_query[task.qid]->vector, vec_data, dist_func_param_);
                res_msg.res.emplace_back(dist, ngh_arr[d]);
              }
            }
          }
        }

        if (tman.node_result_queue[task.machine_id].size() >= POST_RES_MERGE) {
          rdma_comm.post_node_result(tman, task.machine_id);
        }
        if (tman.noncore_sync_msg[task.machine_id].size() >=
            POST_NODESYNC_MERGE) {
          rdma_comm.post_node_sync(tman, task.machine_id);
        }
      }
      /*
        Send NodeResult to origin machine (NodeResult queue).
        Send Sync to core machine (sync queue).
        Send Task to non-core machine (task queue).
      */
      // TODO: check if swap queue may have bugs.
      rdma_comm.post_remote_res_task_sync(tman);
      node_queue.clear();
#ifdef DEBUG
        std::cout << "proc node task over\n";
#endif
    }

    if (node_res_queue.size()) {
#ifdef DEBUG
      std::cout << "proc node res\n";
#endif
      for (NodeResult<dist_t> &res_msg : node_res_queue) {
        // printf("proc %d NodeResult ...\n", res_msg.qid);
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
          // printf("voke q%u\n", res_msg.qid);
          subquery_queue.push_back(res_msg.qid);
          global_query[res_msg.qid]->state = POST_STAGE;
        }
      }
      node_res_queue.clear();
#ifdef DEBUG
      std::cout << "proc node res over\n";
#endif
    }

    if (task_queue.size()) {
#ifdef DEBUG
      std::cout << "proc task queue\n";
#endif
#ifdef PROFILER
      m_profiler.start("g_task_Q");
#endif
      // printf("deal task queue\n");
      for (ComputeTask<dist_t> &task : task_queue) {
        if (task.machine_id == anns_param.machine_id) {  // local task.
          global_query[task.qid]->cur_task = &task;
          global_query[task.qid]->has_cur_task = true;
#ifdef DEBUG
          printf("local task\n");
#endif
          // printf("local task from qid: %u\n", task.qid);
          if (!global_query[task.qid]->result_taken &&
              global_query[task.qid]->coro()) {
            take_query_result(tman, ret, task.qid);
          }

        } else {
          // #ifdef PROFILER
          //           // printf("start q%u task_remote timer\n", task.qid);
          //           m_profiler.start("g_task_remote");
          //           m_profiler.count("g_task_remote_n", task.deg);
          // #endif
          // printf("remote task from qid: %u\n", task.qid);
          tman.result_queue[task.machine_id].emplace_back(task.qid, task.core_cnt, task.nocore_cnt);
          ResultMsg<dist_t> &res_msg =
              tman.result_queue[task.machine_id].back();
          // ResultMsg<dist_t> res_msg(task.qid);
          uint32_t *ngh_arr;
          // uint32_t mptr[INLINE_DEG_BAR];
#ifdef DEBUG
          printf("remote task q%u deg:%u\n", task.qid, task.deg);
#endif
          if (task.deg <= INLINE_DEG_BAR) {
            // printf("low deg task\n");
            // for (uint32_t d = 0; d < task.deg; d++) {
            //   // tmp_ptr[d] = vid;
            //   // printf("task.offset %llu\n", task.offset);
            //   mptr[d] = (uint32_t)(task.offset & 0xffffffff);
            //   task.offset >>= 32LL;
            // }
            // ngh_arr = mptr;

            ngh_arr = (uint32_t *)(&task.offset);
          } else {
            // printf(
            //     "task offset %llu qid %llu deg %llu machine_id %llu\n",
            //     task.offset, task.qid, task.deg, task.machine_id);
            ngh_arr = graph_index.scala_graph->scala_graph_v3_get_ngh_ptr(
                task.offset);
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
            //   printf("low deg d%d ngh%d\n", d, ngh_arr[d]);
            // }
            _mm_prefetch(
                vec_ptr + ((*(ngh_arr + d + 1)) - part_ofs) * data_label_size,
                _MM_HINT_T0);
            if (!global_query[task.qid]->visit_hash.CheckAndSet(ngh_arr[d])) {
#ifdef PROF_COMPUTATION
              computation_cnt ++;
#endif
#ifdef COMM_PROFILE
              comm_ctx->work_dist++;
#endif
              char *vec_data = vec_ptr + ((*(ngh_arr + d)) - part_ofs) * data_label_size;
              dist_t dist = fstdistfunc_(
                  global_query[task.qid]->vector, vec_data, dist_func_param_);
              // ef_fill: the issuer's top-ef was not full, or the task came
              // from node expansion and carries no bound. Keep everything.
              if (task.ef_fill || dist < task.lower_bound) {
                res_msg.res.emplace_back(dist, ngh_arr[d]);
              }
            }
          }
          // tman.result_queue[task.machine_id].push_back(res_msg);
          if (tman.result_queue[task.machine_id].size() >= POST_RES_MERGE) {
            // #ifdef PROFILER
            //             m_profiler.start("g_post_result");
            // #endif

            rdma_comm.post_remote_result(tman, task.machine_id);
            // #ifdef PROFILER
            //             m_profiler.end("g_post_result");
            // #endif
          }

          // #ifdef PROFILER
          //           m_profiler.end("g_task_remote");
          // #endif
          // rdma_comm.post_result(res_msg, task.machine_id);
        }
      }
      // #ifdef PROFILER
      //       m_profiler.start("g_post_result");
      // #endif
      // printf("post res\n");
      rdma_comm.post_remote_result(tman);
      // printf("post res over\n");
      // #ifdef PROFILER
      //       m_profiler.end("g_post_result");
      // #endif
      task_queue.clear();
#ifdef PROFILER
      m_profiler.end("g_task_Q");
#endif
#ifdef DEBUG
      std::cout << "proc task queue over\n";
#endif
    }

    // async_queue only store the query in pre-stage.
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

        // printf("local task from qid: %u\n", task.qid);
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

    // proc result queue
    if (result_queue.size()) {
#ifdef PROFILER
      m_profiler.start("g_result_Q");
#endif
#ifdef DEBUG
      printf("proc result queue\n");
#endif
      for (ResultMsg<dist_t> &res_msg : result_queue) {
        // if query is core, dont voke.
        // printf("proc res q%u \n", res_msg.qid);
        if (global_query[res_msg.qid]->state == PRE_STAGE) {
          // printf("push q%u PRE STAGE local res\n", res_msg.qid);
          global_query[res_msg.qid]->cur_res = &res_msg;
          global_query[res_msg.qid]->has_cur_res = true;
          // printf("recv res.....\n");
          if (!global_query[res_msg.qid]->result_taken &&
              global_query[res_msg.qid]->coro()) {
            take_query_result(tman, ret, res_msg.qid);
          }
        } else {
          // printf("push q%u POST STAGE local res\n", res_msg.qid);
          if (global_query[res_msg.qid]->state == END) {
            printf("WARN: q%u is END when recv res\n", res_msg.qid);
            abort();
          }
          global_query[res_msg.qid]->local_res.push_back(res_msg);
          if (global_query[res_msg.qid]->state == PAUSE) {
            // printf("voke q%u\n", res_msg.qid);
            subquery_queue.push_back(res_msg.qid);
            global_query[res_msg.qid]->state = POST_STAGE;
          }
          // printf("push q%u post-stage res over\n", res_msg.qid);
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

    /**
     * Proc local sub-query.
     */
    if (subquery_queue.size()) {
#ifdef PROFILER
      m_profiler.start("g_subquery_Q");
#endif
      for (uint32_t &sqid : subquery_queue) {
        if (!global_query[sqid]->result_taken &&
            global_query[sqid]->coro()) {
          take_query_result(tman, ret, sqid);
        }
      }
      subquery_queue.clear();
#ifdef PROFILER
      m_profiler.end("g_subquery_Q");
#endif
    }
  }

  /**
   * ScalaSearch v3:
   * Try to improve throughput & latency:
   * 1. sub-query
   * ScalaSearch v4:
   * 2. compute-migration
   * 3. load balance sechduler
   *
   * query = corobj()
   * coro[QUERY_NUM]
   */
  std::vector<QueryMsg<dist_t> *> scala_search_sub(size_t qid) {
#ifdef PROFILER
    Profiler &m_profiler = *all_profiler.getLocal();
    m_profiler.start("g_overall");
#endif

    std::vector<QueryMsg<dist_t> *> ret;
    TaskManager<dist_t> &tman = *task_ma.getLocal();

    global_query[qid]->coro = coro_sub_query_v5(qid);

    // uint32_t res_cnt = 0;
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
      rdma_comm.poll_task_result(tman, &m_profiler);
#else
      rdma_comm.poll_task_result(tman);
#endif
      // printf("tman.cur_query_num %u\n", tman.cur_query_num);

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
        scala_sub_proc_v5(tman, ret);
      }
    }

    // Should never be here.
    return ret;
  }

  std::vector<QueryMsg<dist_t> *> scala_search_sub_wait_finish() {
    std::vector<QueryMsg<dist_t> *> ret;
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
    rdma_comm.poll_task_result(tman, &m_profiler);
#else
    rdma_comm.poll_task_result(tman);
#endif

    if (query_queue.size() || result_queue.size() || task_queue.size() ||
        async_queue.size() || subquery_queue.size() || node_queue.size()) {
      scala_sub_proc_v5(tman, ret);
    } else {
      work_steal();
    }

    return ret;
  }

  void scala_search_sub_standby(uint32_t query_num) {
#ifdef PROFILER
    Profiler &m_profiler = *all_profiler.getLocal();
    m_profiler.start("g_overall");
#endif
    std::vector<QueryMsg<dist_t> *> ret;
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
      rdma_comm.poll_task_result(tman, &m_profiler);
#else
      rdma_comm.poll_task_result(tman);
#endif

      if (query_queue.size() || result_queue.size() || task_queue.size() ||
          async_queue.size() || subquery_queue.size() || node_queue.size()) {
        scala_sub_proc_v5(tman, ret);
      } else {
        work_steal();
      }
    }
#ifdef PROFILER
    m_profiler.end("g_overall");
#endif
  }
};

}  // namespace hnswlib
