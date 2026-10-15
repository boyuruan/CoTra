#include "index/index_param.h"

#include "program_options.h"

IndexParameter::IndexParameter(int argc, char **argv) {
  int machine_id_flag = -1;
  _meta_data_ratio = META_DATA_RATIO;
  num_parts = MACHINE_NUM;

  std::cout << "Using meta_data_ratio: " << _meta_data_ratio << std::endl;
  po::options_description desc{program_options_utils::make_program_description(
      "scala_index", "Build CoTra partitioned index.")};
  try {
    desc.add_options()("help,h", "Print information on arguments");

    po::options_description required_configs("Required");
    required_configs.add_options()(
        "data_type", po::value<std::string>(&data_type)->required(),
        program_options_utils::DATA_TYPE_DESCRIPTION);
    required_configs.add_options()(
        "dist_fn", po::value<std::string>(&dist_fn)->required(),
        program_options_utils::DISTANCE_FUNCTION_DESCRIPTION);
    required_configs.add_options()(
        "index_path_prefix",
        po::value<std::string>(&index_path_prefix)->required(),
        program_options_utils::INDEX_PATH_PREFIX_DESCRIPTION);
    required_configs.add_options()(
        "data_path", po::value<std::string>(&data_path)->required(),
        program_options_utils::INPUT_DATA_PATH);
    required_configs.add_options()(
        "search_DRAM_budget,B", po::value<float>(&B)->required(),
        "DRAM budget in GB for searching the index");
    required_configs.add_options()(
        "build_DRAM_budget,M", po::value<float>(&M)->required(),
        "DRAM budget in GB for building the index");
    required_configs.add_options()(
        "thread_num,t", po::value<uint32_t>(&thread_num)->required(),
        "Worker thread count");

    po::options_description optional_configs("Optional");
    optional_configs.add_options()(
        "graph_type",
        po::value<std::string>(&graph_type_str)->default_value("scalagraph_v3"),
        "Graph storage format (scalagraph_v3)");
    optional_configs.add_options()(
        "config_file", po::value<std::string>(&config_file)->default_value(""),
        "Optional legacy cluster config file (unused in standalone shm mode)");
    optional_configs.add_options()(
        "million_num,s",
        po::value<uint32_t>(&subset_size_milllions)->default_value(0),
        "If non-zero, must equal the data-file point count in millions. "
        "Zero uses the count in the file header.");
    optional_configs.add_options()(
        "num_threads,T",
        po::value<uint32_t>(&num_threads)->default_value(omp_get_num_procs()),
        program_options_utils::NUMBER_THREADS_DESCRIPTION);
    optional_configs.add_options()(
        "max_degree,R", po::value<uint32_t>(&max_degree)->default_value(64),
        program_options_utils::MAX_BUILD_DEGREE);
    optional_configs.add_options()(
        "Lbuild,L", po::value<uint32_t>(&L)->default_value(100),
        program_options_utils::GRAPH_BUILD_COMPLEXITY);
    optional_configs.add_options()(
        "QD", po::value<uint32_t>(&QD)->default_value(0),
        " Quantized Dimension for compression");
    optional_configs.add_options()(
        "codebook_prefix",
        po::value<std::string>(&codebook_prefix)->default_value(""),
        "Path prefix for pre-trained codebook");
    optional_configs.add_options()(
        "PQ_disk_bytes", po::value<uint32_t>(&disk_PQ)->default_value(0),
        "Number of bytes to which vectors should be compressed "
        "on SSD; 0 for no compression");
    optional_configs.add_options()(
        "append_reorder_data", po::bool_switch()->default_value(false),
        "Include full precision data in the index.");
    optional_configs.add_options()(
        "build_PQ_bytes", po::value<uint32_t>(&build_PQ)->default_value(0),
        program_options_utils::BUIlD_GRAPH_PQ_BYTES);
    optional_configs.add_options()(
        "use_opq", po::bool_switch()->default_value(false),
        program_options_utils::USE_OPQ);
    optional_configs.add_options()(
        "label_file", po::value<std::string>(&label_file)->default_value(""),
        program_options_utils::LABEL_FILE);
    optional_configs.add_options()(
        "universal_label",
        po::value<std::string>(&universal_label)->default_value(""),
        program_options_utils::UNIVERSAL_LABEL);
    optional_configs.add_options()(
        "FilteredLbuild", po::value<uint32_t>(&Lf)->default_value(0),
        program_options_utils::FILTERED_LBUILD);
    optional_configs.add_options()(
        "filter_threshold,F",
        po::value<uint32_t>(&filter_threshold)->default_value(0),
        "Threshold to break up the existing nodes.");
    optional_configs.add_options()(
        "label_type",
        po::value<std::string>(&label_type)->default_value("uint"),
        program_options_utils::LABEL_TYPE_DESCRIPTION);
    optional_configs.add_options()(
        "meta_data_ratio",
        po::value<float>(&_meta_data_ratio)->default_value(0.01),
        "Ratio of meta data to be stored in meta_index");
    optional_configs.add_options()(
        "scalagraph_v2", po::bool_switch()->default_value(false),
        "ScalaGraph format");
    optional_configs.add_options()(
        "scalagraph_v3", po::bool_switch()->default_value(false),
        "ScalaGraph format");
    optional_configs.add_options()(
        "scala_v3", po::bool_switch()->default_value(false), "Search version");
    optional_configs.add_options()(
        "topindex_deg", po::value<uint32_t>(&topindex_deg)->default_value(16),
        "Top HNSW index degree");
    optional_configs.add_options()(
        "sample_percent",
        po::value<std::string>(&top_sample_percent)->default_value("0.01"),
        "Top index sampling rate");
    optional_configs.add_options()(
        "machine_id,m", po::value<int>(&machine_id_flag)->default_value(-1),
        "Machine ID in [0, MACHINE_NUM)");

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
    if (vm["append_reorder_data"].as<bool>()) append_reorder_data = true;
    if (vm["use_opq"].as<bool>()) use_opq = true;
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << '\n';
    abort();
  }

  if (machine_id_flag < 0 || machine_id_flag >= MACHINE_NUM) {
    std::cerr << "Error: -m/--machine_id must be in [0, " << MACHINE_NUM
              << "), got " << machine_id_flag << "\n";
    abort();
  }
  machine_id = static_cast<uint32_t>(machine_id_flag);

  if (dist_fn == std::string("l2"))
    metric = diskann::Metric::L2;
  else if (dist_fn == std::string("mips"))
    metric = diskann::Metric::INNER_PRODUCT;
  else if (dist_fn == std::string("cosine"))
    metric = diskann::Metric::COSINE;
  else {
    std::cout << "Error. Only l2, mips, and cosine distance functions are "
                 "supported"
              << std::endl;
    abort();
  }

  if (graph_type_str == std::string("scalagraph_v3")) {
    graph_type = SCALA_V3;
  } else {
    std::cerr << "Error: only --graph_type scalagraph_v3 is supported in "
                 "standalone mode.\n";
    abort();
  }

  disk_pq_dims = 0;
  use_disk_pq = false;
  build_pq_bytes = 0;

  data_file_to_use = data_path;
  labels_file_original = label_file;
  index_prefix_path = index_path_prefix;
  labels_file_to_use = index_prefix_path + "_label_formatted.txt";
  pq_pivots_path_base = codebook_prefix;
  pq_pivots_path = file_exists(pq_pivots_path_base)
                       ? pq_pivots_path_base + "_pq_pivots.bin"
                       : index_prefix_path + "_pq_pivots.bin";
  pq_compressed_vectors_path = index_prefix_path + "_pq_compressed.bin";
  mem_index_path = index_prefix_path + "_mem.index";
  final_index_path =
      index_prefix_path + std::to_string(machine_id) + "_final.index";
  final_b1_index_path =
      index_prefix_path + std::to_string(machine_id) + "_final_b1.index";
  final_scala_index_path =
      index_prefix_path + std::to_string(machine_id) + "_final_scala.index";
  final_scala_data_path =
      index_prefix_path + std::to_string(machine_id) + "_final_scala.data";
  disk_index_path = index_prefix_path + "_disk.index";
  medoids_path = disk_index_path + "_medoids.bin";
  centroids_path = disk_index_path + "_centroids.bin";

  labels_to_medoids_path = disk_index_path + "_labels_to_medoids.txt";
  mem_labels_file = mem_index_path + "_labels.txt";
  disk_labels_file = disk_index_path + "_labels.txt";
  mem_univ_label_file = mem_index_path + "_universal_label.txt";
  disk_univ_label_file = disk_index_path + "_universal_label.txt";
  disk_labels_int_map_file = disk_index_path + "_labels_map.txt";
  dummy_remap_file = disk_index_path + "_dummy_remap.txt";

  sample_base_prefix = index_prefix_path + "_sample";
  disk_pq_pivots_path = index_prefix_path + "_disk.index_pq_pivots.bin";
  disk_pq_compressed_vectors_path =
      index_prefix_path + "_disk.index_pq_compressed.bin";
  prepped_base = index_prefix_path + "_prepped_base.bin";
  created_temp_file_for_processed_data = false;

  final_index_universal_label_file = mem_index_path + "_universal_label.txt";

  merged_index_prefix = mem_index_path + "_tempFiles";
  merged_meta_index_prefix = merged_index_prefix + "_meta_index";

  diskann::get_bin_metadata(data_path, vec_num, dim);
  max_vid = vec_num;
  if (subset_size_milllions != 0) {
    uint64_t declared =
        static_cast<uint64_t>(subset_size_milllions) * 1000000ull;
    if (declared != vec_num) {
      std::cerr << "Missmatch parameter and data file.. -s "
                << subset_size_milllions << " file count " << vec_num << "\n";
      abort();
    }
  }

  for (size_t i = 0; i < num_parts; i++) {
    final_part_scala_index_path[i] =
        index_prefix_path + std::to_string(i) + "_final_scala.index";
    final_part_scala_data_path[i] =
        index_prefix_path + std::to_string(i) + "_final_scala.data";
    final_part_index_path[i] =
        index_prefix_path + std::to_string(i) + "_final.index";
    final_part_b1_index_path[i] =
        index_prefix_path + std::to_string(i) + "_final_b1.index";
    part_replica_idmap_filename[i] = merged_index_prefix + "_subshard-" +
                                     std::to_string(i) + "_ids_uint32.bin";
    part_noreplica_idmap_filename[i] = merged_meta_index_prefix + "_subshard-" +
                                       std::to_string(i) + "_ids_uint32.bin";
    part_replica_base_filename[i] =
        merged_index_prefix + "_subshard-" + std::to_string(i) + ".bin";
    part_noreplica_base_filename[i] =
        merged_meta_index_prefix + "_subshard-" + std::to_string(i) + ".bin";

    part_replica_labels_filename[i] =
        merged_index_prefix + "_subshard-" + std::to_string(i) + "_labels.txt";
    part_noreplica_labels_filename[i] = merged_meta_index_prefix +
                                        "_subshard-" + std::to_string(i) +
                                        "_labels.txt";

    part_replica_index_filename[i] =
        merged_index_prefix + "_subshard-" + std::to_string(i) + "_mem.index";
  }
  local_replica_base_file = part_replica_base_filename[machine_id];
  local_noreplica_base_file = part_noreplica_base_filename[machine_id];
  local_replica_idmap_filename = part_replica_idmap_filename[machine_id];
  local_noreplica_idmap_filename = part_noreplica_idmap_filename[machine_id];
  local_replica_labels_file = part_replica_labels_filename[machine_id];
  local_noreplica_labels_file = part_noreplica_labels_filename[machine_id];
  local_index_file = part_replica_index_filename[machine_id];
  temp_label2pos_map_file = index_prefix_path + "_temp_label2pos.map";
  temp_pos2label_map_file = index_prefix_path + "_temp_pos2label.map";
  temp_id2part_map_file = index_prefix_path + "_temp_id2part.map";
  top_index_path = index_prefix_path + "_top.index";
  temp_kmeans_data_file = index_prefix_path + "_temp_kmeans.data";

  if (dim == 0) {
    diskann::cerr << "Error. Dimension is 0" << std::endl;
    abort();
  }
  if (data_type == std::string("int8"))
    vec_size = dim * sizeof(int8_t);
  else if (data_type == std::string("uint8"))
    vec_size = dim * sizeof(uint8_t);
  else if (data_type == std::string("float"))
    vec_size = dim * sizeof(float);
  else {
    diskann::cerr << "Error. Unsupported data type" << std::endl;
    abort();
  }

  b1_ele_size =
      (max_degree + 1) * sizeof(uint32_t) + vec_size + sizeof(uint64_t);
}
