#include "index/scala_build.h"

void ScalaBuild::build_local_graph() {
  try {
    if (index_param.label_file != "" && index_param.label_type == "ushort") {
      if (index_param.data_type == std::string("int8"))
        diskann::build_local_partition<int8_t>(index_param);
      else if (index_param.data_type == std::string("uint8"))
        diskann::build_local_partition<uint8_t, uint16_t>(index_param);
      else if (index_param.data_type == std::string("float"))
        diskann::build_local_partition<float, uint16_t>(index_param);
      else {
        diskann::cerr << "Error. Unsupported data type" << std::endl;
        abort();
      }
    } else {
      if (index_param.data_type == std::string("int8"))
        diskann::build_local_partition<int8_t>(index_param);
      else if (index_param.data_type == std::string("uint8"))
        diskann::build_local_partition<uint8_t>(index_param);
      else if (index_param.data_type == std::string("float"))
        diskann::build_local_partition<float>(index_param);
      else {
        diskann::cerr << "Error. Unsupported data type" << std::endl;
        abort();
      }
    }
  } catch (const std::exception &e) {
    std::cout << std::string(e.what()) << std::endl;
    diskann::cerr << "Build Partition Index Failed." << std::endl;
    abort();
  }
  rdma_comm.build_sync();
  std::cout << "All partition index build over...\n";
}
