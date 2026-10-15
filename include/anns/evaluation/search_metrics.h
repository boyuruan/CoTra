#pragma once

#include "rdma/rdma_comm.h"

namespace cotra {

// Text and CSV formats are kept for existing log parsers.
struct SearchMetrics {
  std::string evaluation_save_path;
  std::ofstream evaluation_save_file;

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

  void report_comm_info(RdmaCommunication &rdma_comm, float time) {
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
        "PART_INFO",    "BUILD_SYNC",  "DISPATCH_META", "DISPATCH_ID",
        "DISPATCH_NGH", "SCALA_META",  "SCALA_NGH",     "TASK",
        "RESULT",       "NODE_RESULT", "NODE_SYNC",     "COMPUTE",
        "QUERY",        "CORE_INFO",   "SCHEDULE",      "SYNC",
        "RELEASE",      "TERMINATION", "CLEAR"};
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

  void printQueryBatchInfo(
      RdmaCommunication &rdma_comm, size_t ef_, float time) {
    float recall = 1.0f * rdma_comm.correct / rdma_comm.total;
    float avg_lat = rdma_comm.query_lat_sum / rdma_comm.query_load;
    size_t all_comp_cnt = rdma_comm.all_computation_cnt;
    float qps = 1000000.0 * rdma_comm.query_load / time;
#ifdef PROF_COMPUTATION
    printf(
        "%d \t %.5f \t %.2f \t %.3f \t %llu \n", ef_, recall, avg_lat, qps,
        all_comp_cnt);
#else
    printf("%d \t %.5f \t %.2f \t %.3f \t \n", ef_, recall, avg_lat, qps);
#endif
  }

  void printQueryBatchInfoSave(
      RdmaCommunication &rdma_comm, size_t ef_, float time,
      std::ofstream &out) {
    float recall = 1.0f * rdma_comm.correct / rdma_comm.total;
    float avg_lat = rdma_comm.query_lat_sum / rdma_comm.query_load;
    float qps = 1000000.0 * rdma_comm.query_load / time;
    printf("-------------- Overall info --------------\n");
    printf(
        "correct: %llu total: %llu query load: %llu\n", rdma_comm.correct,
        rdma_comm.total, rdma_comm.query_load);
    printf("%d \t %.5f \t %.2f \t %.3f \t \n", ef_, recall, avg_lat, qps);
    printf("------------------  End ------------------\n");

    out << ef_ << "," << recall << "," << avg_lat << "," << qps << std::endl;
  }
};

}  // namespace cotra
