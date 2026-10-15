#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rdma/rdma_config.h"

// Mutable build artifacts: ownership maps, replica graphs and exchange
// progress. Initialization and lifetime are deliberately kept at the original
// stages.
struct BuildWorkspace {
  size_t local_max_vid;  // local partition max node id
  uint32_t partition_size[MACHINE_NUM];
  // local partition vector number without replica.
  uint32_t local_partition_size;

  uint32_t medoid;  // local partition medoid

  // store the sorted ids with replica in this partition.
  std::vector<uint32_t> local_replica_ids;
  // store the sorted ids (only belong to this part) in this partition.
  std::vector<uint32_t> local_noreplica_ids;
  // store the sorted ids of each partition (with replica).
  std::vector<uint32_t> part_replica_ids[MACHINE_NUM];
  // store the sorted ids of each partition (without replica).
  std::vector<uint32_t> part_noreplica_ids[MACHINE_NUM];

  // kmeans_idmap[vid]: partition id stored the vid (here vid is label).
  uint32_t *kmeans_id2part_map;

  // whether recv all meta for one partition.
  bool recv_meta[MACHINE_NUM];

  /* dispatch & collect ids */
  size_t dispatch_id_num[MACHINE_NUM];  // for send meta
  char *dispatch_ids[MACHINE_NUM];      // for send ids.
  size_t collect_id_num[MACHINE_NUM];   // for recv meta
  size_t collect_id_cnt[MACHINE_NUM];   // for recv count.
  char *collected_ids[MACHINE_NUM];     // recved id to merge.

  /* dispatch & collect nghs */
  size_t dispatch_ngh_num[MACHINE_NUM];  // for send meta
  char *dispatch_nghs[MACHINE_NUM];      // for send nghs.
  size_t collect_ngh_num[MACHINE_NUM];   // for recv meta
  size_t collect_ngh_cnt[MACHINE_NUM];   // for recv count.
  char *collected_nghs[MACHINE_NUM];     // recved ngh to merge.

  // map
  uint32_t *label2pos_map;
  uint32_t *pos2label_map;
};
