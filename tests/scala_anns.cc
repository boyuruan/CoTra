#include "anns/evaluation/recall_runner.h"
#include "index/graph_index.h"

int main(int argc, char **argv) {
  SharedMem coromem;

  commandLine cmd(
      argc, argv,
      "Usage : -d <shm[:tag]> -m <machine_id> --query_path <q> --gt_path <gt> "
      "--index_path_prefix <prefix>\n");

  RdmaParameter rdma_param(cmd);
  IndexParameter index_param(argc, argv);
  AnnsParameter anns_param(argc, argv, index_param);

  GraphIndex graph_index(index_param);

  if (anns_param.dist_fn == std::string("l2")) {
    if (anns_param.data_type_str == std::string("uint8")) {
      anns_param.load_query_gt(anns_param.int_answers);
      ScalaSearch<int> appr_alg(
          anns_param, rdma_param, anns_param.l2_space_i, graph_index);
      appr_alg.set_evaluation_save_path(anns_param.evaluation_save_path);
      run_recall_sweep<int>(anns_param, appr_alg, anns_param.int_answers);
    } else if (anns_param.data_type_str == std::string("int8")) {
      anns_param.load_query_gt(anns_param.int_answers);
      ScalaSearch<int> appr_alg(
          anns_param, rdma_param, anns_param.l2_space_ii, graph_index);
      appr_alg.set_evaluation_save_path(anns_param.evaluation_save_path);
      run_recall_sweep<int>(anns_param, appr_alg, anns_param.int_answers);
    } else if (anns_param.data_type_str == std::string("float")) {
      anns_param.load_query_gt(anns_param.float_answers);
      ScalaSearch<float> appr_alg(
          anns_param, rdma_param, anns_param.l2_space, graph_index);
      appr_alg.set_evaluation_save_path(anns_param.evaluation_save_path);
      run_recall_sweep<float>(anns_param, appr_alg, anns_param.float_answers);
    }
  } else if (anns_param.dist_fn == std::string("mips")) {
    anns_param.load_query_gt(anns_param.float_answers);
    ScalaSearch<float> appr_alg(
        anns_param, rdma_param, anns_param.ip_space, graph_index);
    appr_alg.set_evaluation_save_path(anns_param.evaluation_save_path);
    run_recall_sweep<float>(anns_param, appr_alg, anns_param.float_answers);
  } else {
    std::cerr << "Error unexpected dist function.\n";
    abort();
  }

  cout << "Actual memory usage: " << getCurrentRSS() / 1000000 << " Mb \n";

  return 0;
}
