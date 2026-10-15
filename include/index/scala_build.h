#pragma once

#include "index/disk_utils.h"
#include "index/math_utils.h"
#include "rdma/rdma_comm.h"

// CoTra build stages. Implementations live in src/index/build_*.cc;
// run() preserves the distributed barriers and thread-pool handoff.
class ScalaBuild {
 public:
  IndexParameter index_param;
  RdmaParameter rdma_param;
  RdmaCommunication rdma_comm;

  ScalaBuild(IndexParameter &index_param_, RdmaParameter &rdma_param_)
      : index_param(index_param_), rdma_param(rdma_param_) {
    index_param.show();
  }
  ~ScalaBuild() {}

  void run(commandLine &cmd);
  void initialize_transport();
  void partition_vectors();
  void distribute_partition_metadata();
  void build_local_graph();
  void write_partition_maps();
  void load_partition_maps();
  void merge_replica_graphs();
  void encode_intermediate_graph();
  void encode_partitioned_graph();
  void describe_top_index_inputs();

 private:
  char *for_read_ptr;
  uint32_t for_read_sz;

  int kmeans_partition();
  void send_replica_partition();
  void poll_build();
  void collect_local_ids_nghs();
  size_t serialize_dispatch_meta(char *ptr, uint32_t machine_id);
  void proc_dispatch_meta(char *ptr);
  bool collect_meta_finish();
  size_t serialize_dispatch_ids(char *ptr, size_t &id_num, uint32_t machine_id);
  void proc_dispatch_id(char *ptr);
  size_t serialize_dispatch_nghs(
      char *ptr, size_t &ngh_num, uint32_t machine_id);
  void proc_dispatch_ngh(char *ptr);
  bool collect_id_finish();
  bool collect_ngh_finish();
  void clear_finish();
  void delete_temp_file();
};
