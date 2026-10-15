#include "index/scala_build.h"

void ScalaBuild::merge_replica_graphs() {
  std::cout << "Start to merge ...\n";
  std::cout << "Reorganize replica partitions ...\n";
  diskann::collect_replica_partition(index_param);
  std::cout << "Dispatch replica partitions ...\n";
  send_replica_partition();
  diskann::merge_replica_partition(index_param);
  encode_intermediate_graph();
  rdma_comm.build_sync();
}

void ScalaBuild::send_replica_partition() {
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    if (m == index_param.machine_id) {
      clear_finish();
      collect_local_ids_nghs();

      diskann::cout << "Collect partition " << m << " from other machines ..."
                    << std::endl;
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
      while (id_num < index_param.workspace.dispatch_id_num[m]) {
        uint32_t buf_id = rdma_comm.get_write_send_buf(m);
        size_t len = serialize_dispatch_ids(buf->buffer[m][buf_id], id_num, m);
        rdma_comm.post_write_send(m, buf_id, len, DISPATCH_ID);
      }
      rdma_comm.build_sync();
      diskann::cout << "Partition " << m << " ids Dispatched\n";

      size_t ngh_num = 0;
      while (ngh_num < index_param.workspace.dispatch_ngh_num[m]) {
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

void ScalaBuild::poll_build() {
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

void ScalaBuild::collect_local_ids_nghs() {
  index_param.workspace.collect_id_num[index_param.machine_id] =
      index_param.workspace.dispatch_id_num[index_param.machine_id];
  index_param.workspace.collect_ngh_num[index_param.machine_id] =
      index_param.workspace.dispatch_ngh_num[index_param.machine_id];

  index_param.workspace.collected_ids[index_param.machine_id] =
      index_param.workspace.dispatch_ids[index_param.machine_id];
  index_param.workspace.collected_nghs[index_param.machine_id] =
      index_param.workspace.dispatch_nghs[index_param.machine_id];
}

size_t ScalaBuild::serialize_dispatch_meta(char *ptr, uint32_t machine_id) {
  size_t offset = 0;
  memcpy(ptr + offset, &index_param.machine_id, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  size_t id_num = index_param.workspace.dispatch_id_num[machine_id];
  memcpy(ptr + offset, &id_num, sizeof(size_t));
  offset += sizeof(size_t);
  size_t ngh_num = index_param.workspace.dispatch_ngh_num[machine_id];
  memcpy(ptr + offset, &ngh_num, sizeof(size_t));
  offset += sizeof(size_t);

  printf(
      "send dispatch info to m%u idnum %zu nghnum %zu\n", machine_id, id_num,
      ngh_num);

  return offset;
}

void ScalaBuild::proc_dispatch_meta(char *ptr) {
  size_t offset = 0;
  uint32_t machine_id;
  memcpy(&machine_id, ptr + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  memcpy(
      &index_param.workspace.collect_id_num[machine_id], ptr + offset,
      sizeof(size_t));
  offset += sizeof(size_t);
  memcpy(
      &index_param.workspace.collect_ngh_num[machine_id], ptr + offset,
      sizeof(size_t));
  offset += sizeof(size_t);

  printf(
      "recv collect info from m%u idnum %zu nghnum %zu\n", machine_id,
      index_param.workspace.collect_id_num[machine_id],
      index_param.workspace.collect_ngh_num[machine_id]);
  index_param.workspace.recv_meta[machine_id] = true;
  index_param.workspace.collect_id_cnt[machine_id] = 0;
  index_param.workspace.collect_ngh_cnt[machine_id] = 0;
  index_param.workspace.collected_ids[machine_id] = (char *)malloc(
      sizeof(uint32_t) * index_param.workspace.collect_id_num[machine_id]);
  index_param.workspace.collected_nghs[machine_id] = (char *)malloc(
      sizeof(uint32_t) * index_param.workspace.collect_ngh_num[machine_id]);
}

bool ScalaBuild::collect_meta_finish() {
  index_param.workspace.recv_meta[index_param.machine_id] = true;
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    if (!index_param.workspace.recv_meta[m]) {
      return false;
    }
  }
  std::cout << "expect partition id size: \n";
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    std::cout << "m " << m << ":" << index_param.workspace.collect_id_num[m]
              << "\n";
  }
  std::cout << "expect partition ngh size: \n";
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    std::cout << "m " << m << ":" << index_param.workspace.collect_ngh_num[m]
              << "\n";
  }
  return true;
}

size_t ScalaBuild::serialize_dispatch_ids(
    char *ptr, size_t &id_num, uint32_t machine_id) {
  size_t offset = 0;
  memcpy(ptr + offset, &index_param.machine_id, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  size_t max_trans_num = std::min(
      (size_t)((MAX_QUERYBUFFER_SIZE - offset - sizeof(size_t)) /
               sizeof(uint32_t)),
      index_param.workspace.dispatch_id_num[machine_id] - id_num);
  memcpy(ptr + offset, &max_trans_num, sizeof(size_t));
  offset += sizeof(size_t);

  memcpy(
      ptr + offset,
      index_param.workspace.dispatch_ids[machine_id] +
          (size_t)id_num * sizeof(uint32_t),
      max_trans_num * sizeof(uint32_t));
  id_num += max_trans_num;
  offset += max_trans_num * sizeof(uint32_t);
  return offset;
}

void ScalaBuild::proc_dispatch_id(char *ptr) {
  size_t offset = 0;
  uint32_t machine_id;
  memcpy(&machine_id, ptr + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  size_t trans_num;
  memcpy(&trans_num, ptr + offset, sizeof(size_t));
  offset += sizeof(size_t);
  memcpy(
      index_param.workspace.collected_ids[machine_id] +
          (size_t)index_param.workspace.collect_id_cnt[machine_id] *
              sizeof(uint32_t),
      ptr + offset, trans_num * sizeof(uint32_t));
  index_param.workspace.collect_id_cnt[machine_id] += trans_num;
  offset += trans_num * sizeof(uint32_t);
}

size_t ScalaBuild::serialize_dispatch_nghs(
    char *ptr, size_t &ngh_num, uint32_t machine_id) {
  size_t offset = 0;
  memcpy(ptr + offset, &index_param.machine_id, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  size_t max_trans_num = std::min(
      (size_t)((MAX_QUERYBUFFER_SIZE - offset - sizeof(size_t)) /
               sizeof(uint32_t)),
      index_param.workspace.dispatch_ngh_num[machine_id] - ngh_num);
  memcpy(ptr + offset, &max_trans_num, sizeof(size_t));
  offset += sizeof(size_t);
  memcpy(
      ptr + offset,
      index_param.workspace.dispatch_nghs[machine_id] +
          (size_t)ngh_num * sizeof(uint32_t),
      max_trans_num * sizeof(uint32_t));
  ngh_num += max_trans_num;
  offset += max_trans_num * sizeof(uint32_t);
  return offset;
}

void ScalaBuild::proc_dispatch_ngh(char *ptr) {
  size_t offset = 0;
  uint32_t machine_id;
  memcpy(&machine_id, ptr + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  size_t trans_num;
  memcpy(&trans_num, ptr + offset, sizeof(size_t));
  offset += sizeof(size_t);
  memcpy(
      index_param.workspace.collected_nghs[machine_id] +
          index_param.workspace.collect_ngh_cnt[machine_id] * sizeof(uint32_t),
      ptr + offset, trans_num * sizeof(uint32_t));
  index_param.workspace.collect_ngh_cnt[machine_id] += trans_num;
  offset += trans_num * sizeof(uint32_t);
}

bool ScalaBuild::collect_id_finish() {
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    if (m == index_param.machine_id) continue;
    if (index_param.workspace.collect_id_cnt[m] <
        index_param.workspace.collect_id_num[m]) {
      return false;
    }
  }
  return true;
}

bool ScalaBuild::collect_ngh_finish() {
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    if (m == index_param.machine_id) continue;
    if (index_param.workspace.collect_ngh_cnt[m] <
        index_param.workspace.collect_ngh_num[m]) {
      return false;
    }
  }
  return true;
}

void ScalaBuild::clear_finish() {
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    index_param.workspace.collect_id_cnt[m] =
        index_param.workspace.collect_ngh_cnt[m] = 0;
    index_param.workspace.recv_meta[m] = false;
  }
}
