#include "index/scala_build.h"

void ScalaBuild::partition_vectors() {
  index_param.replica_num = 2;
  kmeans_partition();
}

int ScalaBuild::kmeans_partition() {
  if (rdma_param.machine_id == 0) {
    try {
      if (index_param.label_file != "" && index_param.label_type == "ushort") {
        if (index_param.data_type == std::string("int8"))
          diskann::kmeans_partition<int8_t>(index_param);
        else if (index_param.data_type == std::string("uint8"))
          diskann::kmeans_partition<uint8_t, uint16_t>(index_param);
        else if (index_param.data_type == std::string("float"))
          diskann::kmeans_partition<float, uint16_t>(index_param);
        else {
          diskann::cerr << "Error. Unsupported data type" << std::endl;
          abort();
        }
      } else {
        if (index_param.data_type == std::string("int8"))
          diskann::kmeans_partition<int8_t>(index_param);
        else if (index_param.data_type == std::string("uint8"))
          diskann::kmeans_partition<uint8_t>(index_param);
        else if (index_param.data_type == std::string("float"))
          diskann::kmeans_partition<float>(index_param);
        else {
          diskann::cerr << "Error. Unsupported data type" << std::endl;
          abort();
        }
      }
    } catch (const std::exception &e) {
      std::cout << std::string(e.what()) << std::endl;
      diskann::cerr << "Kmeans Partition failed." << std::endl;
      abort();
    }
  }
  return 0;
}

void ScalaBuild::distribute_partition_metadata() {
  if (rdma_param.machine_id == 0) {
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == index_param.machine_id) {
        continue;
      }
      rdma_comm.post_partition_info(index_param, m);
    }
    std::cout << "Partition info send over.\n";
  } else {
    std::cout << "Waiting for partition info ...\n";
    rdma_comm.poll_partition_info(index_param);
    std::cout << "Partition info all recved.\n";
  }
}

void ScalaBuild::write_partition_maps() {
  if (rdma_param.machine_id == 0) {
    std::cout << "Write Partition Map Info ...\n";
    std::cout << "max vid: " << index_param.max_vid
              << " dim: " << index_param.dim << "\n";
    index_param.workspace.kmeans_id2part_map =
        (uint32_t *)malloc(sizeof(uint32_t) * (index_param.max_vid));
    memset(
        index_param.workspace.kmeans_id2part_map, 0,
        sizeof(uint32_t) * index_param.max_vid);

    for (uint64_t m = 0; m < index_param.num_parts; m++) {
      diskann::read_idmap(
          index_param.part_noreplica_idmap_filename[m],
          index_param.workspace.part_noreplica_ids[m]);
      index_param.workspace.partition_size[m] =
          index_param.workspace.part_noreplica_ids[m].size();
      std::cout << " no-replica partition " << m
                << " size: " << index_param.workspace.partition_size[m]
                << std::endl;
    }
    for (uint64_t m = 0; m < index_param.num_parts; m++) {
      size_t nelems = index_param.workspace.part_noreplica_ids[m].size();
      printf("noreplica id size: %zu\n", nelems);
      for (uint32_t i = 0; i < nelems; i++) {
        uint32_t id = index_param.workspace.part_noreplica_ids[m][i];
        index_param.workspace.kmeans_id2part_map[id] = m;
      }
    }
    std::ofstream id2part_map_out(
        index_param.temp_id2part_map_file, std::ios::binary);
    id2part_map_out.write((char *)&index_param.max_vid, sizeof(uint32_t));
    id2part_map_out.write((char *)&index_param.num_parts, sizeof(uint32_t));
    id2part_map_out.write(
        (char *)index_param.workspace.kmeans_id2part_map,
        sizeof(uint32_t) * (index_param.max_vid));
    id2part_map_out.close();
    std::cout << "Id to Part Map file write over.\n";
    free(index_param.workspace.kmeans_id2part_map);

    index_param.workspace.label2pos_map =
        (uint32_t *)malloc(index_param.max_vid * sizeof(uint32_t));
    memset(
        index_param.workspace.label2pos_map, 0,
        index_param.max_vid * sizeof(uint32_t));
    index_param.workspace.pos2label_map =
        (uint32_t *)malloc(index_param.max_vid * sizeof(uint32_t));
    memset(
        index_param.workspace.pos2label_map, 0,
        index_param.max_vid * sizeof(uint32_t));

    uint32_t pos = 0;
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      for (uint32_t i = 0;
           i < index_param.workspace.part_noreplica_ids[m].size(); i++) {
        uint32_t v = index_param.workspace.part_noreplica_ids[m][i];
        index_param.workspace.label2pos_map[v] = pos;
        index_param.workspace.pos2label_map[pos] = v;
        pos++;
      }
    }
    std::ofstream p2l_map_out(
        index_param.temp_pos2label_map_file, std::ios::binary);
    p2l_map_out.write((char *)&index_param.max_vid, sizeof(uint32_t));
    p2l_map_out.write((char *)&index_param.num_parts, sizeof(uint32_t));
    for (uint32_t i = 0; i < index_param.num_parts; i++) {
      p2l_map_out.write(
          (char *)&index_param.workspace.partition_size[i], sizeof(uint32_t));
    }
    p2l_map_out.write(
        (char *)index_param.workspace.pos2label_map,
        sizeof(uint32_t) * index_param.max_vid);
    p2l_map_out.close();
    printf("Pos Label Map file write over.\n");

    std::ofstream l2p_map_out(
        index_param.temp_label2pos_map_file, std::ios::binary);
    l2p_map_out.write((char *)&index_param.max_vid, sizeof(uint32_t));
    l2p_map_out.write((char *)&index_param.num_parts, sizeof(uint32_t));
    for (uint32_t i = 0; i < index_param.num_parts; i++) {
      l2p_map_out.write(
          (char *)&index_param.workspace.partition_size[i], sizeof(uint32_t));
    }
    l2p_map_out.write(
        (char *)index_param.workspace.label2pos_map,
        sizeof(uint32_t) * index_param.max_vid);
    l2p_map_out.close();
    std::cout << "Label Pos Map file write over.\n";
    free(index_param.workspace.label2pos_map);
    free(index_param.workspace.pos2label_map);
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      index_param.workspace.part_noreplica_ids[m].clear();
      index_param.workspace.part_replica_ids[m].clear();
    }
    rdma_comm.build_sync();
  } else {
    rdma_comm.build_sync();
  }
}

void ScalaBuild::load_partition_maps() {
  std::cout << "======== Partition Checkpoint BEGIN ========\n";
  diskann::read_idmap(
      index_param.local_noreplica_idmap_filename,
      index_param.workspace.local_noreplica_ids);
  std::cout << " noreplica partition " << index_param.machine_id
            << " size: " << index_param.workspace.local_noreplica_ids.size()
            << std::endl;

  size_t nelems = 0;
  if (index_param.replica_num > 1) {
    diskann::read_idmap(
        index_param.local_replica_idmap_filename,
        index_param.workspace.local_replica_ids);
    std::cout << " replica partition " << index_param.machine_id
              << " size: " << index_param.workspace.local_replica_ids.size()
              << std::endl;

    nelems = index_param.workspace.local_replica_ids.size();
    index_param.workspace.local_max_vid = 0;
    for (uint32_t i = 0; i < nelems; i++) {
      uint32_t id = index_param.workspace.local_replica_ids[i];
      index_param.workspace.local_max_vid =
          std::max(index_param.workspace.local_max_vid, (size_t)id);
    }
  } else {
    nelems = index_param.workspace.local_noreplica_ids.size();
  }

  index_param.workspace.kmeans_id2part_map =
      (uint32_t *)malloc(sizeof(uint32_t) * (index_param.max_vid));
  memset(
      index_param.workspace.kmeans_id2part_map, 0,
      sizeof(uint32_t) * index_param.max_vid);

  std::ifstream id2part_map_in(
      index_param.temp_id2part_map_file, std::ios::binary);
  uint32_t read_max_vid, read_num_parts;
  id2part_map_in.read((char *)&read_max_vid, sizeof(uint32_t));
  id2part_map_in.read((char *)&read_num_parts, sizeof(uint32_t));
  if (read_num_parts != index_param.num_parts) {
    std::cerr << "Error: Id2part map Mismatch of numparts.\n";
    abort();
  }
  id2part_map_in.read(
      (char *)index_param.workspace.kmeans_id2part_map,
      sizeof(uint32_t) * (index_param.max_vid));
  id2part_map_in.close();

  index_param.workspace.label2pos_map =
      (uint32_t *)malloc(index_param.max_vid * sizeof(uint32_t));
  memset(
      index_param.workspace.label2pos_map, 0,
      index_param.max_vid * sizeof(uint32_t));
  index_param.workspace.pos2label_map =
      (uint32_t *)malloc(index_param.max_vid * sizeof(uint32_t));
  memset(
      index_param.workspace.pos2label_map, 0,
      index_param.max_vid * sizeof(uint32_t));

  std::ifstream p2l_map_in(
      index_param.temp_pos2label_map_file, std::ios::binary);
  p2l_map_in.read((char *)&read_max_vid, sizeof(uint32_t));
  p2l_map_in.read((char *)&read_num_parts, sizeof(uint32_t));
  if (read_max_vid != index_param.max_vid ||
      read_num_parts != index_param.num_parts) {
    std::cerr << "Error: label2pos map Mismatch of max vid or numparts.\n";
    abort();
  }
  for (uint32_t i = 0; i < index_param.num_parts; i++) {
    p2l_map_in.read(
        (char *)&index_param.workspace.partition_size[i], sizeof(uint32_t));
  }
  p2l_map_in.read(
      (char *)index_param.workspace.pos2label_map,
      sizeof(uint32_t) * index_param.max_vid);
  p2l_map_in.close();
  printf("Pos Label Map file read over.\n");

  std::ifstream l2p_map_in(
      index_param.temp_label2pos_map_file, std::ios::binary);
  l2p_map_in.read((char *)&read_max_vid, sizeof(uint32_t));
  l2p_map_in.read((char *)&read_num_parts, sizeof(uint32_t));
  if (read_max_vid != index_param.max_vid ||
      read_num_parts != index_param.num_parts) {
    std::cerr << "Error: label2pos map Mismatch of max vid or numparts.\n";
    abort();
  }
  for (uint32_t i = 0; i < index_param.num_parts; i++) {
    l2p_map_in.read(
        (char *)&index_param.workspace.partition_size[i], sizeof(uint32_t));
  }
  l2p_map_in.read(
      (char *)index_param.workspace.label2pos_map,
      sizeof(uint32_t) * index_param.max_vid);
  l2p_map_in.close();
  printf("Label Pos Map file read over.\n");

  diskann::cout << "# max_vid: " << index_param.max_vid
                << ", max_degree: " << index_param.max_degree
                << ", # nodeselems(include replica): " << nelems << std::endl;
  std::cout << "======== Partition Checkpoint END ========\n";
}
