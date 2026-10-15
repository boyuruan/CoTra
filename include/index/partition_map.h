#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Non-owning view: graph IDs are contiguous by partition; labels are the
// original dataset IDs. Loading and lifetime stay with ScalaGraphIndex.
struct PartitionMapView {
  uint32_t partition_num;
  const uint32_t *cumu_partition_num;
  const int *pos_label_map;

  uint32_t machine_of(uint32_t cand_id) const {
    for (uint32_t p = 1; p <= partition_num; p++) {
      if (cumu_partition_num[p] > cand_id) {
        return p - 1;
      }
    }
    fprintf(stderr, "Error: cand_id %u out of partition range\n", cand_id);
    abort();
  }

  uint32_t local_id(uint32_t machine, uint32_t graph_id) const {
    return graph_id - cumu_partition_num[machine];
  }

  int label(uint32_t graph_id) const { return pos_label_map[graph_id]; }
};
