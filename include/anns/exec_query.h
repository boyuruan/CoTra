#pragma once

#include <omp.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <queue>
#include <unordered_set>

#include "anns/scala_search.h"
#include "coromem/include/galois/bag.h"
#include "coromem/include/galois/loops.h"
#include "coromem/include/runtime/range.h"
#include "coromem/include/share_mem.h"
#include "coromem/include/utils.h"
#ifdef DEBUG
const int debug_id = 4;
#endif

using namespace std;
using namespace hnswlib;

template <typename dist_t>
static void test_vs_recall(
    AnnsParameter &anns_param, ScalaSearch<dist_t> &appr_alg,
    vector<std::priority_queue<std::pair<dist_t, labeltype>>> &answers) {
  char *query_ptr = anns_param.query_ptr;
  size_t vecsize = anns_param.vecsize;
  size_t qsize = anns_param.qsize;
  printf("query size: %llu vecsize: %llu\n", qsize, vecsize);
  size_t k = anns_param.res_knn;

#ifdef DEBUG
  vector<size_t> efs = {30, 400};
#else
  vector<size_t> efs = {3,  4,  5,  6,   8,   10,  15,  20,  30,  40,
                        50, 70, 100, 150, 200, 250, 300, 400, 500, 600, 700};
#endif

#if defined(PROF_Q_DISTRI) || defined(PROF_ALL_Q_DISTRI)
  efs = {300};
#endif

  for (size_t ef : efs) {
    appr_alg.set_ef(ef);
    size_t query_load = qsize / MACHINE_NUM;
    size_t load_start = appr_alg.rdma_param.machine_id * query_load;
    std::atomic<size_t> correct(0);
    std::atomic<size_t> total(0);

#ifdef DEBUG
    if (appr_alg.rdma_param.machine_id == 0) {
      query_load = 20;
    } else {
      query_load = 0;
    }
    load_start = 0;
#endif

    appr_alg.initTerm();
    RangeIterator all(query_load);
    on_each(all);
    std::atomic<size_t> load_cnt(0);
#ifdef LAT
    std::chrono::high_resolution_clock::time_point start[qsize];
    std::chrono::high_resolution_clock::time_point end[qsize];
#endif

#ifdef PROFILER
    on_each([&](uint64 tid, uint64 total) {
      auto *local_prof = appr_alg.all_profiler.getLocal();
      Profiler &m_profiler = *local_prof;
      m_profiler.get_mhz();
    });
#endif

    auto proc_res = [&](auto &result) {
      for (auto &res : result) {
#ifdef PROFILER
        auto *local_prof = appr_alg.all_profiler.getLocal();
        Profiler &local_profiler = *local_prof;
        local_profiler += res->profiler;
#endif
        std::priority_queue<std::pair<dist_t, labeltype>> gt(
            answers[res->query_id]);
        unordered_set<labeltype> g;
        total.fetch_add(gt.size());
#ifdef DEBUG
        std::vector<uint32_t> gt_distri;
#endif
        while (gt.size()) {
          g.insert(gt.top().second);
#ifdef DEBUG
          gt_distri.push_back(
              appr_alg.graph_index.scala_graph->get_machine_id(gt.top().second));
#endif
          gt.pop();
        }
#ifdef DEBUG
        printf("end process q%d\n", res->query_id);
        printf("q%u GT machine distribution: \n", res->query_id);
        uint32_t gt_m_cnt[MACHINE_NUM];
        memset(gt_m_cnt, 0, sizeof(gt_m_cnt));
        for (auto m : gt_distri) {
          gt_m_cnt[m]++;
        }
        for (auto m : gt_distri) {
          printf("%u ", gt_m_cnt[m]);
        }
        printf("\n");
#endif
        while (res->result.size()) {
          if (g.find(res->result.top().second) != g.end()) {
            correct.fetch_add(1);
          }
          res->result.pop();
        }
#ifdef LAT
        end[res->query_id] = std::chrono::high_resolution_clock::now();
#endif
#ifdef DEBUG
        printf("query %u Top Cand Distri\n", res->query_id);
        for (uint32_t m = 0; m < MACHINE_NUM; m++) {
          printf("%u ", res->filter_num[m]);
        }
        printf("\n");
        printf("query %u Final Cand Distri\n", res->query_id);
        for (uint32_t m = 0; m < MACHINE_NUM; m++) {
          printf("%u ", res->tmp_m_cnt[m]);
        }
        printf("\n");
#ifdef PROFILER
        printf("q %u report:\n", res->query_id);
        res->profiler.get_mhz();
        res->profiler.report();
#endif
#endif
        load_cnt.fetch_add(1);
      }
      return;
    };

    appr_alg.scala_search_init(query_ptr, vecsize, qsize);

    StopW stopw = StopW();
    float exec_time;
#ifdef PROFILER
    on_each([&](uint64 tid, uint64 total) {
      Profiler &m_profiler = *appr_alg.all_profiler.getLocal();
      m_profiler.start("g_actual");
    });
#endif

    do_all_standby(
        iterate(all),
        [&](const uint32 &i) {
#ifdef DEBUG
          auto tid = ThreadPool::getTID();
          printf("thread %d start process q%d\n", tid, load_start + i);
#endif
#ifdef LAT
          start[load_start + i] = std::chrono::high_resolution_clock::now();
#endif
          auto result = appr_alg.scala_search_sub(load_start + i);
          proc_res(result);
        },
        [&]() {
          int tid = ThreadPool::getTID();
#ifdef DEBUG
          printf("Thread %d waiting for last query.\n", tid);
#endif
#ifdef PROFILER
          Profiler &m_profiler = *appr_alg.all_profiler.getLocal();
          m_profiler.start("g_overall");
#endif
          while (load_cnt.load() < query_load) {
            auto result = appr_alg.scala_search_sub_wait_finish();
            proc_res(result);
          }
#ifdef PROFILER
          m_profiler.end("g_overall");
#endif
          if (tid == 0) {
            double cumu_lat_us = 0.0;
            appr_alg.sendTerm(
                correct.load(), total.load(), query_load, cumu_lat_us);
          }
#ifdef DEBUG
          printf("Thread %d goto standby.\n", tid);
#endif
          appr_alg.scala_search_sub_standby(qsize);

          if (tid == 0) {
            exec_time = stopw.getElapsedTimeMicro();
#ifdef COMM_PROFILE
            appr_alg.report_comm_info(exec_time);
#endif
            if (appr_alg.rdma_param.machine_id == 0) {
              if (anns_param.evaluation_save_path.empty()) {
                appr_alg.printQueryBatchInfo(exec_time);
              } else {
                appr_alg.printQueryBatchInfoSave(
                    exec_time, appr_alg.get_evaluation_save_file());
              }
            }
          }
        },
        steal(), loopname("Reset"));

#ifdef LAT
    double all_lat = 0.0;
    for (uint32_t q = load_start; q < load_start + query_load; q++) {
      all_lat +=
          duration_cast<std::chrono::microseconds>(end[q] - start[q]).count();
    }
    std::cout << "Avg lat: " << all_lat / query_load << "us\n";
#endif

#ifdef PROFILER
    on_each([&](uint64 tid, uint64 total) {
      Profiler &m_profiler = *appr_alg.all_profiler.getLocal();
      m_profiler.end("g_actual");
    });
#endif
    if (appr_alg.rdma_param.machine_id != 0) {
      float recall = 1.0f * correct.load() / total.load();
      float time_us_per_query = exec_time / query_load;
      float qps = 1000000.0 * query_load / exec_time;
      cout << ef << "\t" << recall << "\t" << time_us_per_query << " us\t"
           << qps << " /s\n";
    }
#ifdef PROFILER
#ifdef DEBUG
    if (appr_alg.rdma_param.machine_id != 0) {
      for (uint32_t q = 0; q < 32; q++) {
        printf("q %u report:\n", q);
        appr_alg.global_query[q]->profiler.get_mhz();
        appr_alg.global_query[q]->profiler.report();
      }
    }
#endif
    Profiler over_all_pro;
    over_all_pro.get_mhz();

    for (uint32_t t = 0; t < getActiveThreads(); ++t) {
      auto *local_prof = appr_alg.all_profiler.getRemote(t);
      Profiler &local_profiler = *local_prof;
      over_all_pro += local_profiler;
    }
    over_all_pro.report_sum();
#endif
#ifdef PROF_DEGREE
    printf(
        "local cnt: %llu deg: %llu\n", appr_alg.local_cnt, appr_alg.local_deg);
    printf(
        "remote cnt: %llu deg: %llu\n", appr_alg.remote_cnt,
        appr_alg.remote_deg);
    uint64_t sum_cnt = appr_alg.local_cnt + appr_alg.remote_cnt;
    uint64_t sum_deg = appr_alg.local_deg + appr_alg.remote_deg;
    float l_cnt_ratio = (float)appr_alg.local_cnt * 100.0 / sum_cnt;
    float r_cnt_ratio = (float)appr_alg.remote_cnt * 100.0 / sum_cnt;
    float l_deg_ratio = (float)appr_alg.local_deg * 100.0 / sum_deg;
    float r_deg_ratio = (float)appr_alg.remote_deg * 100.0 / sum_deg;
    printf("local cnt: %.2f%% deg: %.2f%%\n", l_cnt_ratio, l_deg_ratio);
    printf("remote cnt: %.2f%% deg: %.2f%%\n", r_cnt_ratio, r_deg_ratio);

    uint64_t l_sum_dis{0}, r_sum_dis{0};
    for (uint32_t d = 0; d < MAX_GRAPH_DEG; d++) {
      l_sum_dis += appr_alg.local_distri[d];
      r_sum_dis += appr_alg.remote_distri[d];
    }
    printf("deg\t lcnt\t lrat\t rcnt\t rrat\n");
    for (uint32_t d = 0; d < MAX_GRAPH_DEG; d++) {
      printf(
          "%u\t %llu\t %.2f%%\t %llu\t %.2f%%\n", d, appr_alg.local_distri[d],
          appr_alg.local_distri[d] * 100.0 / l_sum_dis,
          appr_alg.remote_distri[d],
          appr_alg.remote_distri[d] * 100.0 / r_sum_dis);
    }
#endif
  }
}
