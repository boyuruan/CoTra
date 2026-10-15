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

  scala_build.run(cmd);

  return 0;
}
