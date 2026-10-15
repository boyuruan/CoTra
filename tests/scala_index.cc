#include <omp.h>

#include <boost/program_options.hpp>

#include "coromem/include/share_mem.h"
#include "coromem/include/utils.h"
#include "index/scala_build.h"
#include "index/top_index.h"

int main(int argc, char **argv) {
  SharedMem coromem;
  commandLine cmd(argc, argv, "Usage : \n");
  RdmaParameter rdma_param(cmd);
  auto &tp = getThreadPool();
  tp.poolPause();

  IndexParameter index_param(argc, argv);

  ScalaBuild scala_build(index_param, rdma_param);

  scala_build.init_rdma();
  scala_build.partition();
  scala_build.swap_partition_info();
  scala_build.build();

  tp.poolContiue();

  scala_build.write_partition_info();
  scala_build.partition_checkpoint();
  scala_build.merge();
  scala_build.transfer_to_scala_v3();

  if (index_param.machine_id == 0) {
    scala_build.topindex_checkpoint();
    HNSWTopIndex top_index;
    top_index.build_hnsw_top_index(scala_build.index_param, cmd);
  }

  return 0;
}
