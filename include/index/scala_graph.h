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
#include "index/index_param.h"
#include "index/partition_map.h"

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
        read_pid, read_pnum, local_vec_num, local_ngh_ele_size, remote_ngh_num,
        enter_p);
    assert(read_pid == partition_id && read_pnum == partition_num);

    size_t index_size =
        local_ngh_ele_size * local_vec_num + remote_ngh_num * sizeof(uint32_t);
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
      if (!std::filesystem::exists(index_param.final_part_scala_data_path[p])) {
        std::cout << "Error: file does not exist" << std::endl;
        throw std::runtime_error("file does not exist");
      }
      std::ifstream data_input(
          index_param.final_part_scala_data_path[p].c_str(), std::ios::binary);
      data_input.read((char *)&read_pid, 4);
      data_input.read((char *)&read_pnum, 4);
      data_input.read((char *)&partition_vec_num[p], 4);
      data_input.read((char *)&all_vec_num, 8);
      printf(
          "partid %u partnum %u part_vec_num %llu all_vec_num %llu\n", read_pid,
          read_pnum, partition_vec_num[p], all_vec_num);
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

  PartitionMapView partition_map() const {
    return {partition_num, cumu_partition_num, kmeans_pos_label_map};
  }

  uint32_t get_machine_id(uint32_t cand_id) {
    return partition_map().machine_of(cand_id);
  }

  // Raw adjacency bytes are read-only after load. The view decodes only
  // the storage header; routing decisions belong to NeighborExpansion.
  char *adjacency_data() const { return deg_ngh; }

  struct NeighborBlock {
    uint8_t end_flag;
    uint8_t local_flag;
    uint32_t machine;
    uint32_t degree;
  };

  NeighborBlock neighbor_block(size_t offset) const {
    uint8_t *ptr = (uint8_t *)(deg_ngh + offset);
    return {
        static_cast<uint8_t>(ptr[1] & 0x80),
        static_cast<uint8_t>(ptr[1] & 0x40),
        static_cast<uint32_t>(ptr[1] & 0x3f), ptr[0]};
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
