#include "index/scala_build.h"

#include "index/top_index.h"

void ScalaBuild::initialize_transport() {
  for_read_sz = 64 * 1024 * 1024;
  for_read_ptr = (char *)malloc(for_read_sz);
  rdma_comm.com_init(for_read_ptr, for_read_sz, rdma_param);
  rdma_comm.com_read_init();
  rdma_comm.com_write_init();
}

void ScalaBuild::describe_top_index_inputs() {
  std::cout << "======== TopIndex Checkpoint BEGIN ========\n";
  std::cout << "max vid: " << index_param.max_vid << " dim: " << index_param.dim
            << "\n";
  std::cout << "pos label map file: " << index_param.temp_pos2label_map_file
            << std::endl;
  std::cout << "label pos map file: " << index_param.temp_label2pos_map_file
            << std::endl;
  std::cout << "======== TopIndex Checkpoint END ========\n";
}

void ScalaBuild::delete_temp_file() {
  std::remove(index_param.local_replica_base_file.c_str());
  std::remove(index_param.local_replica_idmap_filename.c_str());
  std::remove(index_param.local_noreplica_base_file.c_str());
  std::remove(index_param.local_noreplica_idmap_filename.c_str());
}

void ScalaBuild::run(commandLine &cmd) {
  initialize_transport();
  partition_vectors();
  distribute_partition_metadata();
  build_local_graph();

  getThreadPool().poolContiue();

  write_partition_maps();
  load_partition_maps();
  merge_replica_graphs();
  encode_partitioned_graph();

  if (index_param.machine_id == 0) {
    describe_top_index_inputs();
    HNSWTopIndex top_index;
    top_index.build_hnsw_top_index(index_param, cmd);
  }
}
