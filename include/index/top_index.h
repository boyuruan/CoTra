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

#include "anns/hash_table.h"
#include "coromem/include/utils.h"
#include "index/index_param.h"

class HNSWTopIndex {
 public:
  std::string path;
  uint32_t vec_num;
  uint32_t hop_num;
  uint32_t *hoplevel_cnt;
  uint32_t last_level_bar;
  char *vec_ptr;
  size_t size_data_per_element_;
  size_t ngh_ofs;
  size_t label_ofs;

  HNSWTopIndex() {}

  HNSWTopIndex(IndexParameter &index_param) {
    path = std::string(index_param.top_index_path);
    load_top_index();
  }

  HNSWTopIndex(std::string _path) : path(_path) { load_top_index(); }

  void load_top_index() {
    printf("Loading Top index from %s\n", path.c_str());
    if (!std::filesystem::exists(path)) {
      std::cerr << "Top index file not exist. \n";
      abort();
    }
    std::ifstream input(path.c_str(), std::ios::binary);

    input.read((char *)&vec_num, sizeof(uint32_t));
    input.read((char *)&hop_num, sizeof(uint32_t));

    printf("Read vec_num %u hop_num %u\n", vec_num, hop_num);

    hoplevel_cnt = new uint32_t[hop_num];
    for (uint32_t i = 0; i < hop_num; i++) {
      input.read((char *)&hoplevel_cnt[i], sizeof(uint32_t));
    }
    input.read((char *)&size_data_per_element_, sizeof(size_t));
    size_t deg;
    input.read((char *)&deg, sizeof(size_t));
    ngh_ofs = (deg + 1) * sizeof(uint32_t);
    input.read((char *)&label_ofs, sizeof(size_t));

    vec_ptr = (char *)malloc(vec_num * size_data_per_element_);
    input.read(vec_ptr, vec_num * size_data_per_element_);

    printf("Top index vec_num %u hop_num %u\n", vec_num, hop_num);
    last_level_bar = 0;
    for (uint32_t i = 0; i < hop_num; i++) {
      printf("Hop %u cnt %u\n", i, hoplevel_cnt[i]);
      last_level_bar += hoplevel_cnt[i];
    }
    last_level_bar -= hoplevel_cnt[hop_num - 1];
    printf("Last level bar %u\n", last_level_bar);

    input.close();
  }

  /**
   * The top index is built on base vector data and uses the transformed
   * k-means IDs to align with the base index.
   */
  void build_hnsw_top_index(IndexParameter &index_param, commandLine &cmd);

  uint32_t *get_ngh_arr(size_t vid) {
    return (uint32_t *)(vec_ptr + vid * size_data_per_element_);
  }

  char *get_vector(size_t vid) {
    return vec_ptr + vid * size_data_per_element_ + ngh_ofs;
  }

  uint64_t get_global_id(size_t vid) {
    uint64_t *label_ptr =
        (uint64_t *)(vec_ptr + vid * size_data_per_element_ + label_ofs);
    return label_ptr[0];
  }

  bool is_last_level(size_t vid) { return vid > last_level_bar; }
};
