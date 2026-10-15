#pragma once

#include "diskann/include/index.h"
#include "diskann/include/utils.h"
#include "index/disk_utils.h"
#include "index/math_utils.h"
#include "rdma/rdma_comm.h"

/**
 * ScalaBuild: Build ScalaGraph (V3) based on vector data and DiskANN.
 */
class ScalaBuild {
 public:
  char *for_read_ptr;
  uint32_t for_read_sz;

  IndexParameter index_param;

  RdmaParameter rdma_param;
  RdmaCommunication rdma_comm;

  ScalaBuild(IndexParameter &index_param_, RdmaParameter &rdma_param_)
      : index_param(index_param_), rdma_param(rdma_param_) {
    index_param.show();
  }

  ~ScalaBuild() {}

  void init_rdma() {
    for_read_sz = 64 * 1024 * 1024;
    for_read_ptr = (char *)malloc(for_read_sz);
    rdma_comm.com_init(for_read_ptr, for_read_sz, rdma_param);
    rdma_comm.com_read_init();
    rdma_comm.com_write_init();
  }

  void partition() {
    index_param.replica_num = 2;
    kmeans_partition();
  }

  void write_partition_info() {
    if (rdma_param.machine_id == 0) {
      std::cout << "Write Partition Map Info ...\n";
      std::cout << "max vid: " << index_param.max_vid
                << " dim: " << index_param.dim << "\n";
      index_param.kmeans_id2part_map =
          (uint32_t *)malloc(sizeof(uint32_t) * (index_param.max_vid));
      memset(
          index_param.kmeans_id2part_map, 0,
          sizeof(uint32_t) * index_param.max_vid);

      for (uint64_t m = 0; m < index_param.num_parts; m++) {
        diskann::read_idmap(
            index_param.part_noreplica_idmap_filename[m],
            index_param.part_noreplica_ids[m]);
        index_param.partition_size[m] =
            index_param.part_noreplica_ids[m].size();
        std::cout << " no-replica partition " << m
                  << " size: " << index_param.partition_size[m] << std::endl;
      }
      for (uint64_t m = 0; m < index_param.num_parts; m++) {
        size_t nelems = index_param.part_noreplica_ids[m].size();
        printf("noreplica id size: %zu\n", nelems);
        for (uint32_t i = 0; i < nelems; i++) {
          uint32_t id = index_param.part_noreplica_ids[m][i];
          index_param.kmeans_id2part_map[id] = m;
        }
      }
      std::ofstream id2part_map_out(
          index_param.temp_id2part_map_file, std::ios::binary);
      id2part_map_out.write((char *)&index_param.max_vid, sizeof(uint32_t));
      id2part_map_out.write((char *)&index_param.num_parts, sizeof(uint32_t));
      id2part_map_out.write(
          (char *)index_param.kmeans_id2part_map,
          sizeof(uint32_t) * (index_param.max_vid));
      id2part_map_out.close();
      std::cout << "Id to Part Map file write over.\n";
      free(index_param.kmeans_id2part_map);

      index_param.label2pos_map =
          (uint32_t *)malloc(index_param.max_vid * sizeof(uint32_t));
      memset(
          index_param.label2pos_map, 0,
          index_param.max_vid * sizeof(uint32_t));
      index_param.pos2label_map =
          (uint32_t *)malloc(index_param.max_vid * sizeof(uint32_t));
      memset(
          index_param.pos2label_map, 0,
          index_param.max_vid * sizeof(uint32_t));

      uint32_t pos = 0;
      for (uint32_t m = 0; m < MACHINE_NUM; m++) {
        for (uint32_t i = 0; i < index_param.part_noreplica_ids[m].size();
             i++) {
          uint32_t v = index_param.part_noreplica_ids[m][i];
          index_param.label2pos_map[v] = pos;
          index_param.pos2label_map[pos] = v;
          pos++;
        }
      }
      std::ofstream p2l_map_out(
          index_param.temp_pos2label_map_file, std::ios::binary);
      p2l_map_out.write((char *)&index_param.max_vid, sizeof(uint32_t));
      p2l_map_out.write((char *)&index_param.num_parts, sizeof(uint32_t));
      for (uint32_t i = 0; i < index_param.num_parts; i++) {
        p2l_map_out.write(
            (char *)&index_param.partition_size[i], sizeof(uint32_t));
      }
      p2l_map_out.write(
          (char *)index_param.pos2label_map,
          sizeof(uint32_t) * index_param.max_vid);
      p2l_map_out.close();
      printf("Pos Label Map file write over.\n");

      std::ofstream l2p_map_out(
          index_param.temp_label2pos_map_file, std::ios::binary);
      l2p_map_out.write((char *)&index_param.max_vid, sizeof(uint32_t));
      l2p_map_out.write((char *)&index_param.num_parts, sizeof(uint32_t));
      for (uint32_t i = 0; i < index_param.num_parts; i++) {
        l2p_map_out.write(
            (char *)&index_param.partition_size[i], sizeof(uint32_t));
      }
      l2p_map_out.write(
          (char *)index_param.label2pos_map,
          sizeof(uint32_t) * index_param.max_vid);
      l2p_map_out.close();
      std::cout << "Label Pos Map file write over.\n";
      free(index_param.label2pos_map);
      free(index_param.pos2label_map);
      for (uint32_t m = 0; m < MACHINE_NUM; m++) {
        index_param.part_noreplica_ids[m].clear();
        index_param.part_replica_ids[m].clear();
      }
      rdma_comm.build_sync();
    } else {
      rdma_comm.build_sync();
    }
  }

  void swap_partition_info() {
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

  void partition_checkpoint() {
    std::cout << "======== Partition Checkpoint BEGIN ========\n";
    diskann::read_idmap(
        index_param.local_noreplica_idmap_filename,
        index_param.local_noreplica_ids);
    std::cout << " noreplica partition " << index_param.machine_id
              << " size: " << index_param.local_noreplica_ids.size()
              << std::endl;

    size_t nelems = 0;
    if (index_param.replica_num > 1) {
      diskann::read_idmap(
          index_param.local_replica_idmap_filename,
          index_param.local_replica_ids);
      std::cout << " replica partition " << index_param.machine_id
                << " size: " << index_param.local_replica_ids.size()
                << std::endl;

      nelems = index_param.local_replica_ids.size();
      index_param.local_max_vid = 0;
      for (uint32_t i = 0; i < nelems; i++) {
        uint32_t id = index_param.local_replica_ids[i];
        index_param.local_max_vid =
            std::max(index_param.local_max_vid, (size_t)id);
      }
    } else {
      nelems = index_param.local_noreplica_ids.size();
    }

    index_param.kmeans_id2part_map =
        (uint32_t *)malloc(sizeof(uint32_t) * (index_param.max_vid));
    memset(
        index_param.kmeans_id2part_map, 0,
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
        (char *)index_param.kmeans_id2part_map,
        sizeof(uint32_t) * (index_param.max_vid));
    id2part_map_in.close();

    index_param.label2pos_map =
        (uint32_t *)malloc(index_param.max_vid * sizeof(uint32_t));
    memset(
        index_param.label2pos_map, 0,
        index_param.max_vid * sizeof(uint32_t));
    index_param.pos2label_map =
        (uint32_t *)malloc(index_param.max_vid * sizeof(uint32_t));
    memset(
        index_param.pos2label_map, 0,
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
          (char *)&index_param.partition_size[i], sizeof(uint32_t));
    }
    p2l_map_in.read(
        (char *)index_param.pos2label_map,
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
          (char *)&index_param.partition_size[i], sizeof(uint32_t));
    }
    l2p_map_in.read(
        (char *)index_param.label2pos_map,
        sizeof(uint32_t) * index_param.max_vid);
    l2p_map_in.close();
    printf("Label Pos Map file read over.\n");

    diskann::cout << "# max_vid: " << index_param.max_vid
                  << ", max_degree: " << index_param.max_degree
                  << ", # nodeselems(include replica): " << nelems
                  << std::endl;
    std::cout << "======== Partition Checkpoint END ========\n";
  }

  int kmeans_partition() {
    if (rdma_param.machine_id == 0) {
      try {
        if (index_param.label_file != "" &&
            index_param.label_type == "ushort") {
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

  void build() {
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

  void merge() {
    std::cout << "Start to merge ...\n";
    std::cout << "Reorganize replica partitions ...\n";
    diskann::collect_replica_partition(index_param);
    std::cout << "Dispatch replica partitions ...\n";
    send_replica_partition();
    diskann::merge_replica_partition(index_param);
    transfer_to_b1graph_format();
    rdma_comm.build_sync();
  }

  void send_replica_partition() {
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == index_param.machine_id) {
        clear_finish();
        collect_local_ids_nghs();

        diskann::cout << "Collect partition " << m
                      << " from other machines ..." << std::endl;
        while (!collect_meta_finish()) {
          poll_build();
        }
        rdma_comm.build_sync();
        diskann::cout << "Partition " << m << " meta Collected\n";

        while (!collect_id_finish()) {
          poll_build();
        }
        rdma_comm.build_sync();
        diskann::cout << "Partition " << m << " id Collected\n";

        while (!collect_ngh_finish()) {
          poll_build();
        }
        rdma_comm.build_sync();
        diskann::cout << "Partition " << m << " ngh Collected\n";

      } else {
        diskann::cout << "Dispatch partition " << m << " to machine " << m
                      << " ..." << std::endl;

        auto *buf = rdma_comm.send_write_buf.getLocal();
        uint32_t buf_id = rdma_comm.get_write_send_buf(m);
        size_t len = serialize_dispatch_meta(buf->buffer[m][buf_id], m);
        rdma_comm.post_write_send(m, buf_id, len, DISPATCH_META);
        rdma_comm.build_sync();
        diskann::cout << "Partition " << m << " meta Dispatched\n";

        size_t id_num = 0;
        while (id_num < index_param.dispatch_id_num[m]) {
          uint32_t buf_id = rdma_comm.get_write_send_buf(m);
          size_t len =
              serialize_dispatch_ids(buf->buffer[m][buf_id], id_num, m);
          rdma_comm.post_write_send(m, buf_id, len, DISPATCH_ID);
        }
        rdma_comm.build_sync();
        diskann::cout << "Partition " << m << " ids Dispatched\n";

        size_t ngh_num = 0;
        while (ngh_num < index_param.dispatch_ngh_num[m]) {
          uint32_t buf_id = rdma_comm.get_write_send_buf(m);
          size_t len =
              serialize_dispatch_nghs(buf->buffer[m][buf_id], ngh_num, m);
          rdma_comm.post_write_send(m, buf_id, len, DISPATCH_NGH);
        }
        rdma_comm.build_sync();
        diskann::cout << "Partition " << m << " nghs Dispatched\n";
      }
    }
  }

  void poll_build() {
    auto *rbuf = rdma_comm.recv_write_buf.getLocal();

    rdma_comm.poll_send();
    rdma_comm.poll_recv();

    while (rbuf->dispatch_meta_queue.size() > 0) {
      char *meta_ptr = rbuf->dispatch_meta_queue.front();
      rbuf->dispatch_meta_queue.pop_front();
      proc_dispatch_meta(meta_ptr);
      free(meta_ptr);
    }

    while (rbuf->dispatch_id_queue.size() > 0) {
      char *id_ptr = rbuf->dispatch_id_queue.front();
      rbuf->dispatch_id_queue.pop_front();
      proc_dispatch_id(id_ptr);
      free(id_ptr);
    }

    while (rbuf->dispatch_ngh_queue.size() > 0) {
      char *ngh_ptr = rbuf->dispatch_ngh_queue.front();
      rbuf->dispatch_ngh_queue.pop_front();
      proc_dispatch_ngh(ngh_ptr);
      free(ngh_ptr);
    }
  }

  void collect_local_ids_nghs() {
    index_param.collect_id_num[index_param.machine_id] =
        index_param.dispatch_id_num[index_param.machine_id];
    index_param.collect_ngh_num[index_param.machine_id] =
        index_param.dispatch_ngh_num[index_param.machine_id];

    index_param.collected_ids[index_param.machine_id] =
        index_param.dispatch_ids[index_param.machine_id];
    index_param.collected_nghs[index_param.machine_id] =
        index_param.dispatch_nghs[index_param.machine_id];
  }

  size_t serialize_dispatch_meta(char *ptr, uint32_t machine_id) {
    size_t offset = 0;
    memcpy(ptr + offset, &index_param.machine_id, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    size_t id_num = index_param.dispatch_id_num[machine_id];
    memcpy(ptr + offset, &id_num, sizeof(size_t));
    offset += sizeof(size_t);
    size_t ngh_num = index_param.dispatch_ngh_num[machine_id];
    memcpy(ptr + offset, &ngh_num, sizeof(size_t));
    offset += sizeof(size_t);

    printf(
        "send dispatch info to m%u idnum %zu nghnum %zu\n", machine_id, id_num,
        ngh_num);

    return offset;
  }

  void proc_dispatch_meta(char *ptr) {
    size_t offset = 0;
    uint32_t machine_id;
    memcpy(&machine_id, ptr + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    memcpy(
        &index_param.collect_id_num[machine_id], ptr + offset, sizeof(size_t));
    offset += sizeof(size_t);
    memcpy(
        &index_param.collect_ngh_num[machine_id], ptr + offset, sizeof(size_t));
    offset += sizeof(size_t);

    printf(
        "recv collect info from m%u idnum %zu nghnum %zu\n", machine_id,
        index_param.collect_id_num[machine_id],
        index_param.collect_ngh_num[machine_id]);
    index_param.recv_meta[machine_id] = true;
    index_param.collect_id_cnt[machine_id] = 0;
    index_param.collect_ngh_cnt[machine_id] = 0;
    index_param.collected_ids[machine_id] = (char *)malloc(
        sizeof(uint32_t) * index_param.collect_id_num[machine_id]);
    index_param.collected_nghs[machine_id] = (char *)malloc(
        sizeof(uint32_t) * index_param.collect_ngh_num[machine_id]);
  }

  bool collect_meta_finish() {
    index_param.recv_meta[index_param.machine_id] = true;
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (!index_param.recv_meta[m]) {
        return false;
      }
    }
    std::cout << "expect partition id size: \n";
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      std::cout << "m " << m << ":" << index_param.collect_id_num[m] << "\n";
    }
    std::cout << "expect partition ngh size: \n";
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      std::cout << "m " << m << ":" << index_param.collect_ngh_num[m] << "\n";
    }
    return true;
  }

  size_t serialize_dispatch_ids(
      char *ptr, size_t &id_num, uint32_t machine_id) {
    size_t offset = 0;
    memcpy(ptr + offset, &index_param.machine_id, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    size_t max_trans_num = std::min(
        (size_t)((MAX_QUERYBUFFER_SIZE - offset - sizeof(size_t)) /
                 sizeof(uint32_t)),
        index_param.dispatch_id_num[machine_id] - id_num);
    memcpy(ptr + offset, &max_trans_num, sizeof(size_t));
    offset += sizeof(size_t);

    memcpy(
        ptr + offset,
        index_param.dispatch_ids[machine_id] +
            (size_t)id_num * sizeof(uint32_t),
        max_trans_num * sizeof(uint32_t));
    id_num += max_trans_num;
    offset += max_trans_num * sizeof(uint32_t);
    return offset;
  }

  void proc_dispatch_id(char *ptr) {
    size_t offset = 0;
    uint32_t machine_id;
    memcpy(&machine_id, ptr + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    size_t trans_num;
    memcpy(&trans_num, ptr + offset, sizeof(size_t));
    offset += sizeof(size_t);
    memcpy(
        index_param.collected_ids[machine_id] +
            (size_t)index_param.collect_id_cnt[machine_id] * sizeof(uint32_t),
        ptr + offset, trans_num * sizeof(uint32_t));
    index_param.collect_id_cnt[machine_id] += trans_num;
    offset += trans_num * sizeof(uint32_t);
  }

  size_t serialize_dispatch_nghs(
      char *ptr, size_t &ngh_num, uint32_t machine_id) {
    size_t offset = 0;
    memcpy(ptr + offset, &index_param.machine_id, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    size_t max_trans_num = std::min(
        (size_t)((MAX_QUERYBUFFER_SIZE - offset - sizeof(size_t)) /
                 sizeof(uint32_t)),
        index_param.dispatch_ngh_num[machine_id] - ngh_num);
    memcpy(ptr + offset, &max_trans_num, sizeof(size_t));
    offset += sizeof(size_t);
    memcpy(
        ptr + offset,
        index_param.dispatch_nghs[machine_id] +
            (size_t)ngh_num * sizeof(uint32_t),
        max_trans_num * sizeof(uint32_t));
    ngh_num += max_trans_num;
    offset += max_trans_num * sizeof(uint32_t);
    return offset;
  }

  void proc_dispatch_ngh(char *ptr) {
    size_t offset = 0;
    uint32_t machine_id;
    memcpy(&machine_id, ptr + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    size_t trans_num;
    memcpy(&trans_num, ptr + offset, sizeof(size_t));
    offset += sizeof(size_t);
    memcpy(
        index_param.collected_nghs[machine_id] +
            index_param.collect_ngh_cnt[machine_id] * sizeof(uint32_t),
        ptr + offset, trans_num * sizeof(uint32_t));
    index_param.collect_ngh_cnt[machine_id] += trans_num;
    offset += trans_num * sizeof(uint32_t);
  }

  bool collect_id_finish() {
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == index_param.machine_id) continue;
      if (index_param.collect_id_cnt[m] < index_param.collect_id_num[m]) {
        return false;
      }
    }
    return true;
  }

  bool collect_ngh_finish() {
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      if (m == index_param.machine_id) continue;
      if (index_param.collect_ngh_cnt[m] < index_param.collect_ngh_num[m]) {
        return false;
      }
    }
    return true;
  }

  void clear_finish() {
    for (uint32_t m = 0; m < MACHINE_NUM; m++) {
      index_param.collect_id_cnt[m] = index_param.collect_ngh_cnt[m] = 0;
      index_param.recv_meta[m] = false;
    }
  }

  void transfer_to_b1graph_format() {
    std::cout << "Start transfer index to b1graph.\n";
    size_t read_blk_size = 64 * 1024 * 1024;
    if (!std::filesystem::exists(index_param.final_index_path)) {
      std::cerr << "Error: final_index_path does not exist.\n";
      abort();
    }

    cached_ifstream reader(index_param.final_index_path, read_blk_size);
    size_t fsize = reader.get_file_size();
    if (fsize == 0) {
      std::stringstream stream;
      stream << "Error, new file specified is empty. Not appending. "
             << std::endl;
      throw diskann::ANNException(stream.str(), -1);
    }

    char *final_idx_ptr = (char *)malloc(fsize);
    size_t num_blocks = DIV_ROUND_UP(fsize, read_blk_size);
    for (uint64_t i = 0; i < num_blocks; i++) {
      size_t cur_block_size = read_blk_size > fsize - (i * read_blk_size)
                                  ? fsize - (i * read_blk_size)
                                  : read_blk_size;
      reader.read(final_idx_ptr + i * read_blk_size, cur_block_size);
    }

    uint64_t vec_num = index_param.partition_size[index_param.machine_id];
    uint64_t b1_ele_size = sizeof(uint32_t) +
                           index_param.max_degree * sizeof(uint32_t) +
                           index_param.vec_size + sizeof(uint64_t);
    uint64_t merge_graph_size =
        sizeof(uint64_t) + sizeof(index_param.medoid) + b1_ele_size * vec_num;

    size_t merge_meta_offset = 0;
    char *merge_graph = (char *)malloc(merge_graph_size);
    if (merge_graph == nullptr) {
      std::cerr << "Error: failed to malloc b1graph mem.\n";
      abort();
    }

    memcpy(merge_graph + merge_meta_offset, &vec_num, sizeof(vec_num));
    merge_meta_offset += sizeof(vec_num);

    memcpy(
        merge_graph + merge_meta_offset, &index_param.medoid,
        sizeof(index_param.medoid));
    merge_meta_offset += sizeof(index_param.medoid);
    printf(
        "b1graph vnum: %llu medoid %u size %llu\n", vec_num, index_param.medoid,
        merge_graph_size);

    std::ifstream base_reader(
        index_param.local_noreplica_base_file.c_str(), std::ios::binary);
    std::cout << "read base data from: "
              << index_param.local_noreplica_base_file << std::endl;
    uint32_t dummy_size, basedim32;
    base_reader.read((char *)&dummy_size, sizeof(uint32_t));
    base_reader.read((char *)&basedim32, sizeof(uint32_t));

    size_t offset = 0;
    size_t index_size;
    memcpy(&index_size, final_idx_ptr + offset, sizeof(index_size));
    offset += sizeof(index_size);
    uint32_t max_deg;
    memcpy(&max_deg, final_idx_ptr + offset, sizeof(max_deg));
    offset += sizeof(max_deg);
    if (max_deg != index_param.max_degree) {
      std::cerr << "Error: Mismatch of max degree.\n";
      abort();
    }
    uint32_t medoid;
    memcpy(&medoid, final_idx_ptr + offset, sizeof(medoid));
    offset += sizeof(medoid);

    index_param.medoid = medoid;

    uint64_t index_frozen;
    memcpy(&index_frozen, final_idx_ptr + offset, sizeof(index_frozen));
    offset += sizeof(index_frozen);

    uint32_t *ngh_ptr = (uint32_t *)(final_idx_ptr + offset);
    size_t ngh_offset = 0;
    size_t ele_num = 0;
    size_t partition_ofs = 0;
    for (uint32_t m = 0; m < index_param.machine_id; m++) {
      partition_ofs += index_param.partition_size[m];
    }
    std::cout << "partition_ofs: " << partition_ofs << std::endl;
    while (offset + ngh_offset * sizeof(uint32_t) < index_size) {
      char *ele_ptr =
          (char *)(merge_graph + merge_meta_offset + ele_num * b1_ele_size);
      size_t ele_ofs = 0;
      uint32_t degree = ngh_ptr[ngh_offset];
      uint32_t remap_ngh[degree + 1];
      remap_ngh[0] = degree;
      for (uint32_t i = 1; i < degree + 1; i++) {
        remap_ngh[i] = index_param.label2pos_map[ngh_ptr[ngh_offset + i]];
      }
      memcpy(
          ele_ptr + ele_ofs, (char *)remap_ngh,
          (degree + 1) * sizeof(uint32_t));
      ele_ofs += (index_param.max_degree + 1) * sizeof(uint32_t);
      base_reader.read(ele_ptr + ele_ofs, index_param.vec_size);
      ele_ofs += index_param.vec_size;
      size_t pos = partition_ofs + ele_num;
      size_t label = index_param.pos2label_map[pos];
      memcpy(ele_ptr + ele_ofs, (char *)&label, sizeof(size_t));
      ngh_offset += degree + 1;
      ele_num++;
    }
    std::cout << "max deg: " << max_deg << " medoid: " << medoid
              << " index frozen: " << index_frozen << std::endl;
    std::cout << ele_num << " nodes read.\n";

    std::cout << "start write index\n";
    cached_ofstream output(
        index_param.final_b1_index_path, BUFFER_SIZE_FOR_CACHED_IO);
    size_t write_blk_size = 64 * 1024 * 1024;
    num_blocks = DIV_ROUND_UP(merge_graph_size, write_blk_size);
    for (uint64_t i = 0; i < num_blocks; i++) {
      size_t cur_block_size =
          write_blk_size > merge_graph_size - (i * write_blk_size)
              ? merge_graph_size - (i * write_blk_size)
              : write_blk_size;
      output.write(merge_graph + i * write_blk_size, cur_block_size);
    }
    free(final_idx_ptr);
    free(merge_graph);
    std::cout << " b1 graph vnum num:" << ele_num << "\n";
    std::cout << "Transfer index to b1graph over: "
              << index_param.final_b1_index_path << "\n";
  }

  void transfer_to_scala_v3();

  void topindex_checkpoint() {
    std::cout << "======== TopIndex Checkpoint BEGIN ========\n";
    std::cout << "max vid: " << index_param.max_vid
              << " dim: " << index_param.dim << "\n";
    std::cout << "pos label map file: " << index_param.temp_pos2label_map_file
              << std::endl;
    std::cout << "label pos map file: " << index_param.temp_label2pos_map_file
              << std::endl;
    std::cout << "======== TopIndex Checkpoint END ========\n";
  }

  void delete_temp_file() {
    std::remove(index_param.local_replica_base_file.c_str());
    std::remove(index_param.local_replica_idmap_filename.c_str());
    std::remove(index_param.local_noreplica_base_file.c_str());
    std::remove(index_param.local_noreplica_idmap_filename.c_str());
  }
};
