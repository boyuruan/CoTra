#pragma once

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "anns/anns_param.h"
#include "anns/pq_codes.h"
#include "anns/task_manager.h"
#include "index/index_param.h"
#include "worklists/balance_queue.h"

inline void get_pos_label_map(
    const char *pos_label_path, int &vec_size, int &part_num, int *&part_size,
    int *&pos_label_map) {
  if (!std::filesystem::exists(pos_label_path)) {
    printf("Pos Label Map file does not exist !!!\n");
    std::abort();
  }
  std::ifstream map_in(pos_label_path, std::ios::binary);
  map_in.read((char *)&vec_size, sizeof(uint32_t));
  printf("vec_size %d\n", vec_size);
  map_in.read((char *)&part_num, sizeof(uint32_t));
  printf("part_num %d\n", part_num);

  part_size = (int *)malloc(sizeof(uint32_t) * part_num);
  map_in.read((char *)part_size, sizeof(uint32_t) * part_num);
  for (int i = 0; i < part_num; i++) {
    printf("%d ", part_size[i]);
  }
  printf("\n");
  pos_label_map = (int *)malloc(sizeof(uint32_t) * vec_size);
  map_in.read((char *)pos_label_map, sizeof(uint32_t) * vec_size);
  map_in.close();
}

/**
 * ScalaGraphIndex: Uniform index storage for ScalaSearch (V3 format).
 */
class ScalaGraphIndex {
  IndexParameter &index_param;
  char *deg_ngh{nullptr};

 public:
  char *vector_ptr{nullptr};
  char *enter_p_ptr{nullptr};

  uint64_t all_vec_num{0};
  uint64_t deg_ngh_size{0};
  uint32_t partition_num{0};
  uint32_t partition_id{0};
  uint32_t *partition_vec_num{nullptr};
  uint32_t *cumu_partition_num{nullptr};
  uint32_t local_part_vec_num{0};
  uint64_t partition_ofs{0};
  uint32_t subset_size_milllions{0};
  uint64_t enter_p{0};

  uint32_t local_vec_num{0};
  size_t local_ngh_ele_size{0};
  uint64_t remote_ngh_num{0};

  int *kmeans_label_pos_map{nullptr};
  int *kmeans_pos_label_map{nullptr};

  // PQ gate on inline neighbors held by another machine. With pq set and
  // top-ef full, a neighbor whose PQ distance exceeds lowerBound * pq_scale
  // is not sent. pq_shadow counts those neighbors and sends them anyway.
  const PQCodes *pq{nullptr};
  float pq_scale{1.0f};
  bool pq_shadow{false};

  ScalaGraphIndex(IndexParameter &_index_param) : index_param(_index_param) {
    partition_num = index_param.num_parts;
    partition_id = index_param.machine_id;
    partition_vec_num = (uint32_t *)malloc(sizeof(uint32_t) * partition_num);
    cumu_partition_num =
        (uint32_t *)malloc(sizeof(uint32_t) * (partition_num + 1));
    subset_size_milllions = index_param.subset_size_milllions;
  }

  ~ScalaGraphIndex() {}

  void load_index() {
    printf("ScalaGraph V3 loading index ...\n");

    int *kmeans_part_size;
    int kmeans_vec_size, kmeans_part_num;
    get_pos_label_map(
        index_param.temp_pos2label_map_file.c_str(), kmeans_vec_size,
        kmeans_part_num, kmeans_part_size, kmeans_pos_label_map);

    printf(
        "Loading partition index %d from %s \n", partition_id,
        index_param.final_scala_index_path.c_str());

    if (!std::filesystem::exists(index_param.final_scala_index_path)) {
      std::cout << "Error: file does not exist" << std::endl;
      throw std::runtime_error("file does not exist");
    }
    std::ifstream idx_input(
        index_param.final_scala_index_path.c_str(), std::ios::binary);

    /*
      [ScalaGraph V3 index format]:
      <partition_id: uint32_t>, <partition_num: uint32_t>,
      <part_vec_num: uint32_t>, <local_ngh_ele_size: uint32_t>,
      <enter_point: uint64_t>,
      <remote_ngh_num: uint64_t>
      (header 32 bytes)
      <local_ngh_array: local_ngh_ele_size> * part_vec_num,
      <remote_ngh_array: uint32_t> * remote_ngh_num.
    */
    uint32_t read_pid, read_pnum;
    idx_input.read((char *)&read_pid, sizeof(uint32_t));
    idx_input.read((char *)&read_pnum, sizeof(uint32_t));
    idx_input.read((char *)&local_vec_num, sizeof(uint32_t));
    uint32_t read_local_ngh_ele_size;
    idx_input.read((char *)&read_local_ngh_ele_size, sizeof(uint32_t));
    local_ngh_ele_size = read_local_ngh_ele_size;
    idx_input.read((char *)&enter_p, sizeof(uint64_t));
    idx_input.read((char *)&remote_ngh_num, sizeof(uint64_t));
    printf(
        "partid %u partnum %u local_vec_num %llu "
        "local_ngh_ele_size %llu remote_ngh_num %llu enter_point %llu\n",
        read_pid, read_pnum, local_vec_num, local_ngh_ele_size,
        remote_ngh_num, enter_p);
    assert(read_pid == partition_id && read_pnum == partition_num);

    size_t index_size = local_ngh_ele_size * local_vec_num +
                        remote_ngh_num * sizeof(uint32_t);
    deg_ngh_size = index_size;
    printf("read v3 index size %llu\n", index_size);
    deg_ngh = (char *)malloc(index_size);
    idx_input.read((char *)deg_ngh, index_size);
    idx_input.close();

    /*
      Data format:
      <partition_id: uint32_t>, <partition_num: uint32_t>,
      <partition_vec_num: uint32_t>, <all_vec_num: uint64_t>,
      (header 20 bytes)
      <enterpoint vector: dim * type + label>
      <vector: dim * type + label> * partition_vec_num.
    */
    for (uint32_t p = 0; p < partition_num; p++) {
      printf(
          "Loading partition %u meta data form %s\n", p,
          index_param.final_part_scala_data_path[p].c_str());
      if (!std::filesystem::exists(
              index_param.final_part_scala_data_path[p])) {
        std::cout << "Error: file does not exist" << std::endl;
        throw std::runtime_error("file does not exist");
      }
      std::ifstream data_input(
          index_param.final_part_scala_data_path[p].c_str(),
          std::ios::binary);
      data_input.read((char *)&read_pid, 4);
      data_input.read((char *)&read_pnum, 4);
      data_input.read((char *)&partition_vec_num[p], 4);
      data_input.read((char *)&all_vec_num, 8);
      printf(
          "partid %u partnum %u part_vec_num %llu all_vec_num %llu\n",
          read_pid, read_pnum, partition_vec_num[p], all_vec_num);
      if (p == partition_id) {
        enter_p_ptr = (char *)malloc(index_param.vec_size + sizeof(uint64_t));
        data_input.read(
            (char *)enter_p_ptr, index_param.vec_size + sizeof(uint64_t));

        vector_ptr = (char *)malloc(
            (index_param.vec_size + sizeof(uint64_t)) *
            partition_vec_num[partition_id]);
        data_input.read(
            (char *)vector_ptr, (index_param.vec_size + sizeof(uint64_t)) *
                                    partition_vec_num[partition_id]);
      }
      data_input.close();
    }
    cumu_partition_num[0] = 0;
    for (uint32_t p = 1; p <= partition_num; p++) {
      cumu_partition_num[p] =
          cumu_partition_num[p - 1] + partition_vec_num[p - 1];
    }
    partition_ofs = cumu_partition_num[partition_id];
    printf("partition_ofs: %u\n", partition_ofs);
    local_part_vec_num = partition_vec_num[partition_id];

    printf("ScalaGraph V3 Load Over\n");
  }

  uint32_t get_machine_id(uint32_t cand_id) {
    for (uint32_t p = 1; p <= partition_num; p++) {
      if (cumu_partition_num[p] > cand_id) {
        return p - 1;
      }
    }
    fprintf(stderr, "Error: cand_id %u out of partition range\n", cand_id);
    abort();
  }

  char *get_partition_ptr() { return vector_ptr; }

  size_t get_partition_size() {
    return (index_param.vec_size + sizeof(uint64_t)) *
           partition_vec_num[partition_id];
  }

  inline uint32_t *scala_graph_get_ngh_ptr(uint64_t offset) {
    if (offset + sizeof(uint32_t) > deg_ngh_size) {
      printf(
          "Error: offset %llu exceed deg_ngh_size: %llu\n", offset,
          deg_ngh_size);
      abort();
    }
    return (uint32_t *)(deg_ngh + offset + sizeof(uint32_t));
  }

  inline uint32_t *scala_graph_v3_get_ngh_ptr(uint64_t offset) {
    if (offset > deg_ngh_size) {
      printf(
          "Error: offset %llu exceed deg_ngh_size: %llu\n", offset,
          deg_ngh_size);
      abort();
    }
    return (uint32_t *)(deg_ngh + offset);
  }

  uint32_t scala_graph_get_id(uint64_t offset, uint32_t d) {
    uint32_t *remote_ptr = (uint32_t *)(deg_ngh + offset);
    return remote_ptr[d + 1];
  }

  uint32_t scala_graph_get_internal_id(uint32_t machine_id, uint32_t vid) {
    return vid - cumu_partition_num[machine_id];
  }

  /**
   * Identify local core tasks, remote core tasks, remote non-core tasks.
   */
  template <typename dist_t>
  uint32_t scala_graph_v3_get_subquery_task_lb(
      uint32_t current_node_id, uint32_t query_id, TaskManager<dist_t> &tman,
      BalanceQueue<ComputeTask<dist_t>> &ib, uint64_t *gated = nullptr,
      uint64_t *pq_dist_cnt = nullptr) {
    if (current_node_id < partition_ofs ||
        current_node_id >= partition_ofs + local_part_vec_num) {
      printf(
          "Error: current_node_id %u less or exceed partition_ofs %u\n",
          current_node_id, partition_ofs);
      abort();
    }
    size_t ofs = local_ngh_ele_size * (current_node_id - partition_ofs);
    _mm_prefetch(deg_ngh + ofs, _MM_HINT_T0);
    uint32_t post_cnt = 0;
    dist_t lb = tman.global_query[query_id]->lowerBound;
    uint8_t ef_fill =
        (tman.global_query[query_id]->top_candidates.size() <
         tman.global_query[query_id]->ef);

    const char *qvec = tman.global_query[query_id]->vector;
    float pq_bound = (float)lb * pq_scale;
    auto pq_drop = [&](uint32_t vid, int chan) {
      if (pq == nullptr || ef_fill) return false;
      if (pq_dist_cnt != nullptr) (*pq_dist_cnt)++;
      if (pq->dist(qvec, vid) <= pq_bound) return false;
      if (gated != nullptr) gated[chan]++;
      return !pq_shadow;
    };

    while (true) {
      uint8_t *ptr = (uint8_t *)(deg_ngh + ofs);
      uint8_t end_flag = ptr[1] & 0x80;
      uint8_t local_flag = ptr[1] & 0x40;
      uint32_t pid = ptr[1] & 0x3f;
      uint32_t deg = ptr[0];

      ofs += 2;
      if (local_flag) {
        // after previous stage, will put into local task queue.
        ib.emplace_back(partition_id, query_id, deg, ofs);
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
                  scala_graph_get_internal_id(pid, ngh_ptr[d]);
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
                partition_id, query_id, deg, ofs_ptr[0], lb, ef_fill);
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
  void scala_graph_v3_get_nodetask_msg(
      TaskManager<dist_t> &tman, NodeTask &task, NodeResult<dist_t> &res_msg) {
    size_t ofs = local_ngh_ele_size * (task.node_id - partition_ofs);
    _mm_prefetch(deg_ngh + ofs, _MM_HINT_T0);
    res_msg.post_cnt = 0;

    while (true) {
      uint8_t *ptr = (uint8_t *)(deg_ngh + ofs);
      uint8_t end_flag = ptr[1] & 0x80;
      uint8_t local_flag = ptr[1] & 0x40;
      uint32_t pid = ptr[1] & 0x3f;
      uint32_t deg = ptr[0];
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
                task.machine_id, task.qid, deg, tmp_ofs_for_micro_combine, 0,
                1, 0, 1);  // no bound: ef_fill 1; core count +0 nocore count +1
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

  inline uint32_t scala_graph_get_partition_ofs() { return partition_ofs; }

  char *scala_graph_get_data(uint64_t candidate_id) {
    if (candidate_id < partition_ofs ||
        partition_ofs + local_part_vec_num <= candidate_id) {
      printf(
          "IndexError: candidate_id out of range. vid:%u ofs:%u\n",
          candidate_id, partition_ofs);
      abort();
    }
    uint64_t data_ofs = candidate_id - partition_ofs;
    return (char *)(vector_ptr +
                    (index_param.vec_size + sizeof(uint64_t)) * data_ofs);
  }
};
