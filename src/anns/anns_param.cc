#include "anns/anns_param.h"

#include "program_options.h"

AnnsParameter::AnnsParameter(
    int argc, char **argv, IndexParameter &index_param) {
  int machine_id_flag = -1;

  po::options_description desc{program_options_utils::make_program_description(
      "scala_anns", "CoTra shared-memory vector search.")};
  try {
    desc.add_options()("help,h", "Print information on arguments");

    po::options_description required_configs("Required");
    required_configs.add_options()(
        "data_type", po::value<std::string>(&data_type_str)->required(),
        program_options_utils::DATA_TYPE_DESCRIPTION);
    required_configs.add_options()(
        "dist_fn", po::value<std::string>(&dist_fn)->required(),
        program_options_utils::DISTANCE_FUNCTION_DESCRIPTION);
    required_configs.add_options()(
        "thread_num,t", po::value<uint32_t>(&thd_num)->required(),
        "Worker thread count");
    required_configs.add_options()(
        "query_path", po::value<std::string>(&query_path)->required(),
        "Query binary path");
    required_configs.add_options()(
        "gt_path", po::value<std::string>(&gt_path)->required(),
        "Ground-truth binary path");

    po::options_description optional_configs("Optional");
    optional_configs.add_options()(
        "config_file", po::value<std::string>(&config_file)->default_value(""),
        "Optional legacy cluster config file (unused in standalone shm mode)");
    optional_configs.add_options()(
        "app_type",
        po::value<std::string>(&app_type_str)->default_value("scala_v3"),
        "Search application type (scala_v3)");
    optional_configs.add_options()(
        "res_knn", po::value<uint32_t>(&res_knn)->default_value(10),
        "default knn size is 10");
    optional_configs.add_options()(
        "machine_id,m", po::value<int>(&machine_id_flag)->default_value(-1),
        "Machine ID in [0, MACHINE_NUM)");
    optional_configs.add_options()(
        "max_degree,R", po::value<uint32_t>(&M)->default_value(64),
        program_options_utils::MAX_BUILD_DEGREE);
    optional_configs.add_options()(
        "query_size,Q", po::value<size_t>(&qsize)->default_value(10000),
        "default query size is 10000");
    optional_configs.add_options()(
        "million_num,s",
        po::value<uint32_t>(&subset_size_milllions)->default_value(0),
        "If non-zero, must equal the data-file point count in millions. "
        "Zero uses the count in the file header.");
    optional_configs.add_options()(
        "scala_v3", po::bool_switch()->default_value(false), "Search version");
    optional_configs.add_options()(
        "pq_prefix", po::value<std::string>(&pq_prefix)->default_value(""),
        "scala_v3: PQ files <prefix>_pivots.bin and <prefix>_codes.bin; "
        "empty disables the PQ gate.");
    optional_configs.add_options()(
        "pq_tau", po::value<float>(&pq_tau)->default_value(0.0f),
        "scala_v3: skip an inline remote neighbor when its PQ distance "
        "exceeds lowerBound * (1 + pq_tau). Negative: count only.");

    desc.add(required_configs).add(optional_configs);

    po::variables_map vm;
    auto parsed = po::command_line_parser(argc, argv)
                      .options(desc)
                      .allow_unregistered()
                      .run();
    po::store(parsed, vm);
    if (vm.count("help")) {
      std::cout << desc;
      abort();
    }
    po::notify(vm);
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << '\n';
    abort();
  }

  if (machine_id_flag < 0 || machine_id_flag >= MACHINE_NUM) {
    std::cerr << "Error: -m/--machine_id must be in [0, " << MACHINE_NUM
              << "), got " << machine_id_flag << "\n";
    abort();
  }
  machine_id = machine_id_flag;
  vecsize = index_param.vec_size;
  if (subset_size_milllions == 0) {
    vecnum = index_param.vec_num;
  } else {
    vecnum = static_cast<uint64_t>(subset_size_milllions) * 1000000ull;
    if (vecnum != index_param.vec_num) {
      std::cerr << "Missmatch parameter and data file.. -s "
                << subset_size_milllions << " file count "
                << index_param.vec_num << "\n";
      abort();
    }
  }
  vecdim = index_param.dim;

  auto numThreads = setActiveThreads(thd_num);
  printf("Set thread num = %d\n", numThreads);
  printf(
      "Run CoTra-SHM machine number: %d (local machine_id=%d)\n", MACHINE_NUM,
      machine_id);

  if (dist_fn == std::string("l2")) {
    if (data_type_str == std::string("uint8"))
      l2_space_i = new L2SpaceI(vecdim);
    else if (data_type_str == std::string("int8")) {
      l2_space_ii = new L2SpaceII(vecdim);
    } else
      l2_space = new L2Space(vecdim);
  } else if (dist_fn == std::string("mips"))
    ip_space = new InnerProductSpace(vecdim);
  else {
    std::cout << "Error. Only l2 and mips distance functions are supported"
              << std::endl;
    abort();
  }

  if (app_type_str == std::string("scala_v3")) {
    app_type = ScalaANN_v3;
  } else {
    std::cerr << "Error: only --app_type scala_v3 is supported in standalone "
                 "mode.\n";
    abort();
  }
}
