#include "rdma/rdma_comm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <chrono>

static std::string shm_base_prefix(const RdmaParameter &param) {
  if (param.ib_devname != nullptr && param.ib_devname[0] != '\0' &&
      strcmp(param.ib_devname, "shm") != 0) {
    if (strncmp(param.ib_devname, "shm:", 4) == 0 &&
        param.ib_devname[4] != '\0') {
      std::string tag(param.ib_devname + 4);
      if (tag[0] == '/') return tag;
      return "/dev/shm/" + tag;
    }
    fprintf(
        stderr,
        "Invalid -d '%s': standalone build supports only 'shm' or 'shm:<tag>'\n",
        param.ib_devname);
    abort();
  }
  struct statvfs vfs;
  if (statvfs("/dev/shm", &vfs) == 0 &&
      uint64_t(vfs.f_blocks) * vfs.f_frsize >= (512ULL << 20)) {
    return "/dev/shm/cotra_shm";
  }
  return "/tmp/cotra_shm";
}

static std::string shm_peer_path(const std::string &prefix, int n, int m) {
  return prefix + "_n" + std::to_string(n) + "_m" + std::to_string(m);
}

RdmaCommunication::~RdmaCommunication() { shm_cleanup(); }

void RdmaCommunication::shm_cleanup() {
  if (!shm_my_path.empty()) {
    unlink(shm_my_path.c_str());
    shm_my_path.clear();
  }
  for (int m = 0; m < MACHINE_NUM; m++) {
    if (shm_peer_base[m] != nullptr) {
      munmap(shm_peer_base[m], shm_peer_size[m]);
      shm_peer_base[m] = nullptr;
      shm_peer_size[m] = 0;
    }
  }
}

int RdmaCommunication::ctx_close_connection() {
  shm_cleanup();
  return 0;
}

void RdmaCommunication::com_init_shm(
    char *vec_part_ptr, size_t vec_part_len) {
  int my_id = rdma_param.machine_id;
  int n = rdma_param.machine_num;
  int num_threads = getActiveThreads();
  rdma_param.machine = (my_id == 0) ? LEADER : MEMBER;

  size_t part_offset = ROUND_UP(sizeof(ShmSegmentHeader), 4096);
  size_t part_alloc = ROUND_UP(vec_part_len > 0 ? vec_part_len : 4096, 4096);
  size_t thread_region_offset = part_offset + part_alloc;
  size_t total_bytes =
      thread_region_offset + size_t(num_threads) * sizeof(ShmThreadRegion);

  std::string prefix = shm_base_prefix(rdma_param);
  shm_my_path = shm_peer_path(prefix, n, my_id);
  unlink(shm_my_path.c_str());

  int fd = open(shm_my_path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
  if (fd < 0) {
    perror("com_init_shm: open");
    exit(1);
  }
  if (ftruncate(fd, total_bytes) != 0) {
    perror("com_init_shm: ftruncate");
    close(fd);
    unlink(shm_my_path.c_str());
    exit(1);
  }
  void *my_base =
      mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (my_base == MAP_FAILED) {
    perror("com_init_shm: mmap");
    unlink(shm_my_path.c_str());
    exit(1);
  }

  shm_peer_base[my_id] = my_base;
  shm_peer_size[my_id] = total_bytes;

  memset(my_base, 0, part_offset);
  if (vec_part_ptr != nullptr && vec_part_len > 0) {
    memcpy(
        static_cast<char *>(my_base) + part_offset, vec_part_ptr, vec_part_len);
  }
  memset(
      static_cast<char *>(my_base) + thread_region_offset, 0,
      size_t(num_threads) * sizeof(ShmThreadRegion));

  auto *my_hdr = static_cast<ShmSegmentHeader *>(my_base);
  my_hdr->machine_id = my_id;
  my_hdr->machine_num = n;
  my_hdr->thread_num = num_threads;
  my_hdr->part_offset = part_offset;
  my_hdr->part_len = vec_part_len;
  my_hdr->thread_region_offset = thread_region_offset;
  my_hdr->total_bytes = total_bytes;

  uint64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
  uint64_t my_iid =
      (uint64_t(getpid()) << 32) ^ (now_ns & 0xffffffffULL) ^
      (uint64_t(my_id + 1) << 56);
  if (my_iid == 0) my_iid = 1;

  my_hdr->ack_from[my_id].store(my_iid, std::memory_order_relaxed);
  my_hdr->ack_target[my_id].store(my_iid, std::memory_order_relaxed);
  my_hdr->instance_id.store(my_iid, std::memory_order_release);
  my_hdr->magic.store(COTRA_SHM_MAGIC, std::memory_order_release);

  ino_t peer_ino[MACHINE_NUM] = {0};
  for (;;) {
    bool all_acked = true;
    for (int m = 0; m < n; m++) {
      if (m == my_id) continue;
      std::string ppath = shm_peer_path(prefix, n, m);
      struct stat st;
      if (stat(ppath.c_str(), &st) != 0 ||
          st.st_size < static_cast<off_t>(sizeof(ShmSegmentHeader))) {
        all_acked = false;
        continue;
      }
      if (shm_peer_base[m] != nullptr && peer_ino[m] != st.st_ino) {
        munmap(shm_peer_base[m], shm_peer_size[m]);
        shm_peer_base[m] = nullptr;
        shm_peer_size[m] = 0;
      }
      if (shm_peer_base[m] == nullptr) {
        int pfd = open(ppath.c_str(), O_RDWR);
        if (pfd < 0) {
          all_acked = false;
          continue;
        }
        struct stat fst;
        if (fstat(pfd, &fst) != 0 ||
            fst.st_size < static_cast<off_t>(sizeof(ShmSegmentHeader))) {
          close(pfd);
          all_acked = false;
          continue;
        }
        void *hdr_map = mmap(
            nullptr, sizeof(ShmSegmentHeader), PROT_READ | PROT_WRITE,
            MAP_SHARED, pfd, 0);
        if (hdr_map == MAP_FAILED) {
          close(pfd);
          all_acked = false;
          continue;
        }
        auto *phdr_probe = static_cast<ShmSegmentHeader *>(hdr_map);
        uint64_t magic = phdr_probe->magic.load(std::memory_order_acquire);
        uint64_t ptotal = phdr_probe->total_bytes;
        munmap(hdr_map, sizeof(ShmSegmentHeader));
        if (magic != COTRA_SHM_MAGIC ||
            fst.st_size < static_cast<off_t>(ptotal)) {
          close(pfd);
          all_acked = false;
          continue;
        }
        void *pmap =
            mmap(nullptr, ptotal, PROT_READ | PROT_WRITE, MAP_SHARED, pfd, 0);
        close(pfd);
        if (pmap == MAP_FAILED) {
          all_acked = false;
          continue;
        }
        shm_peer_base[m] = pmap;
        shm_peer_size[m] = ptotal;
        peer_ino[m] = fst.st_ino;
      }

      auto *phdr = static_cast<ShmSegmentHeader *>(shm_peer_base[m]);
      if (phdr->magic.load(std::memory_order_acquire) != COTRA_SHM_MAGIC) {
        all_acked = false;
        continue;
      }
      if (phdr->machine_id != static_cast<uint32_t>(m) ||
          phdr->machine_num != static_cast<uint32_t>(n) ||
          phdr->thread_num != static_cast<uint32_t>(num_threads) ||
          phdr->thread_region_offset +
                  size_t(num_threads) * sizeof(ShmThreadRegion) !=
              phdr->total_bytes) {
        fprintf(
            stderr,
            "com_init_shm: peer m%d header mismatch (machine_num=%u "
            "thread_num=%u)\n",
            m, phdr->machine_num, phdr->thread_num);
        abort();
      }
      uint64_t peer_iid = phdr->instance_id.load(std::memory_order_acquire);
      phdr->ack_from[my_id].store(my_iid, std::memory_order_release);
      phdr->ack_target[my_id].store(peer_iid, std::memory_order_release);

      if (my_hdr->ack_from[m].load(std::memory_order_acquire) != peer_iid ||
          my_hdr->ack_target[m].load(std::memory_order_acquire) != my_iid) {
        all_acked = false;
      }
    }
    if (all_acked) break;
    usleep(1000);
  }

  my_hdr->all_connected.store(1, std::memory_order_release);
  for (int m = 0; m < n; m++) {
    auto *phdr = static_cast<ShmSegmentHeader *>(shm_peer_base[m]);
    while (phdr->all_connected.load(std::memory_order_acquire) == 0) {
      usleep(500);
    }
  }
  unlink(shm_my_path.c_str());
  shm_my_path.clear();

  on_each([&](uint64 tid, uint64 total) {
    auto *local_buf = read_buf.getLocal();
    auto *local_send_write = send_write_buf.getLocal();
    auto *local_recv_write = recv_write_buf.getLocal();
    auto *ctx = rdma_ctx.getLocal();

    memset(ctx, 0, sizeof(RdmaContext));
    ctx->is_shm_mode = true;
    ctx->thread_id = tid;
    ctx->machine_num = n;
    ctx->machine_id = my_id;
    ctx->cache_line_size = rdma_param.cache_line_size;
    ctx->shm_my_thread = reinterpret_cast<ShmThreadRegion *>(
        static_cast<char *>(my_base) + my_hdr->thread_region_offset +
        size_t(tid) * sizeof(ShmThreadRegion));

    for (int m = 0; m < n; m++) {
      auto *phdr = static_cast<ShmSegmentHeader *>(shm_peer_base[m]);
      ctx->shm_peer_part[m] =
          static_cast<char *>(shm_peer_base[m]) + phdr->part_offset;
      ctx->shm_peer_thread[m] = reinterpret_cast<ShmThreadRegion *>(
          static_cast<char *>(shm_peer_base[m]) + phdr->thread_region_offset +
          size_t(tid) * sizeof(ShmThreadRegion));
      ctx->for_read_ptr[m] = vec_part_ptr;
      ctx->remote_mr_msg[m].read_vaddr =
          reinterpret_cast<uintptr_t>(ctx->shm_peer_part[m]);
      ctx->remote_mr_msg[m].write_vaddr = reinterpret_cast<uintptr_t>(
          &ctx->shm_peer_thread[m]->recv_buf[my_id][0][0]);
    }

    local_send_write->init();
    local_recv_write->init(&ctx->shm_my_thread->recv_buf[0][0][0]);

    for (size_t b = 0; b < MAX_CACHE_VEC; b++) {
      ctx->read_to_ptr[b] = local_buf->vector[b];
    }
    memset(ctx->read_to_ptr[0], 0, MAX_CACHE_VEC * MAX_VEC_LEN);
    for (int m = 0; m < n; m++) {
      for (size_t b = 0; b < MAX_WRITE_NUM; b++) {
        ctx->send_write_ptr[m][b] = local_send_write->buffer[m][b];
        ctx->recv_write_ptr[m][b] = local_recv_write->buffer[m][b];
      }
    }
  });

  printf("SHM RDMA communication init over.\n");
}

void RdmaCommunication::com_init(
    char *vec_part_ptr, size_t vec_part_len, RdmaParameter &input_param) {
  rdma_param = input_param;
  com_init_shm(vec_part_ptr, vec_part_len);
}

/**
 * Poll send cq one time.
 */
int RdmaCommunication::poll_send() {
  auto *ctx = rdma_ctx.getLocal();

  int ne = ctx->poll_SEND();
  if (ne > 0) {
    for (int i = 0; i < ne; i++) {
      auto &wc = ctx->s_wc[i];
      if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "poll_send: Completion with error at client\n ");
        fprintf(
            stderr, " Failed status %d: qid %d buff %d syndrom 0x%x\n",
            wc.status, (int)wc.wr_id >> 16, (int)(wc.wr_id & LOW_16BIT_MASK),
            wc.vendor_err);
        return -1;
      }
      uint64_t wr_id = wc.wr_id;
      if (wc.opcode == IBV_WC_RDMA_READ) {
        ctx->collect_num++;
        auto *rd_buf = read_buf.getLocal();
        rd_buf->recv_list.emplace_back(wr_id);  // qid | buf_id
      } else if (wc.opcode == IBV_WC_RDMA_WRITE) {
      } else if (wc.opcode == IBV_WC_SEND) {
        printf("!!! recv SEND\n");
      } else {
        printf("Error: Unexpected opcode.\n");
      }
    }
  }
  return ne;
}

/**
 * Poll recv cq one time.
 */
int RdmaCommunication::poll_recv() {
  auto *ctx = rdma_ctx.getLocal();

  int ne = ctx->poll_RECV();
  if (ne > 0) {
    for (int i = 0; i < ne; i++) {
      auto &wc = ctx->r_wc[i];
      if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "poll_recv: Completion with error at client\n ");
        fprintf(
            stderr,
            " Failed status %d: machine %d true_wr_id %d syndrom 0x%x\n",
            wc.status, (int)wc.wr_id >> 16, (int)(wc.wr_id & LOW_16BIT_MASK),
            wc.vendor_err);
        return -1;
      }

      uint64_t wr_id = wc.wr_id;
      if (wc.opcode == IBV_WC_RECV) {
      } else if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
        auto *rbuf = recv_write_buf.getLocal();
        auto *sbuf = send_write_buf.getLocal();

        uint32_t m_id = wr_id >> 16;
        uint32_t true_wr_id = wr_id & LOW_16BIT_MASK;
        uint32_t data_size = wc.byte_len;
        uint32_t imm = ntohl(wc.imm_data);
        ControlType control = ControlType(imm >> 16);
        uint32_t buf_id = imm & LOW_16BIT_MASK;
        char *ptr = (char *)rbuf->buffer[m_id][buf_id];

        if (control == RELEASE || control == CLEAR) {
          int *release_ptr = (int *)ptr;
          int num = release_ptr[0];
          for (int i = 1; i < num + 1; i++) {
            sbuf->buff_id_list[m_id]->push_back(release_ptr[i]);
          }
        } else if (control == PART_INFO) {
          char *part_info = (char *)malloc(data_size);
          memcpy(part_info, ptr, data_size);
          rbuf->part_info_queue.push_back(part_info);
        } else if (control == BUILD_SYNC) {
          char *sync_info = (char *)malloc(data_size);
          memcpy(sync_info, ptr, data_size);
          rbuf->build_sync_queue.push_back(sync_info);
        } else if (control == DISPATCH_META) {
          char *meta_info = (char *)malloc(data_size);
          memcpy(meta_info, ptr, data_size);
          rbuf->dispatch_meta_queue.push_back(meta_info);
        } else if (control == DISPATCH_ID) {
          char *dispatch_info = (char *)malloc(data_size);
          memcpy(dispatch_info, ptr, data_size);
          rbuf->dispatch_id_queue.push_back(dispatch_info);
        } else if (control == DISPATCH_NGH) {
          char *dispatch_info = (char *)malloc(data_size);
          memcpy(dispatch_info, ptr, data_size);
          rbuf->dispatch_ngh_queue.push_back(dispatch_info);
        } else if (control == SCALA_META) {
          char *scala_meta_info = (char *)malloc(data_size);
          memcpy(scala_meta_info, ptr, data_size);
          rbuf->scala_meta_queue.push_back(scala_meta_info);
        } else if (control == SCALA_NGH) {
          char *scala_ngh_info = (char *)malloc(data_size);
          memcpy(scala_ngh_info, ptr, data_size);
          rbuf->scala_ngh_queue.push_back(scala_ngh_info);
        } else if (control == CORE_INFO) {
          char *core_info = (char *)malloc(data_size);
          memcpy(core_info, ptr, data_size);
          rbuf->core_info_queue.push_back(core_info);
        } else if (control == QUERY) {
          char *new_query = (char *)malloc(data_size);
          memcpy(new_query, ptr, data_size);
          rbuf->migrate_queue.push_back(new_query);
        } else if (control == SYNC) {
          char *sync = (char *)malloc(data_size);
          memcpy(sync, ptr, data_size);
          rbuf->sync_queue.push_back(sync);
        } else if (control == TASK) {
          char *new_task = (char *)malloc(data_size);
          memcpy(new_task, ptr, data_size);
          rbuf->recv_task.push_back(new_task);
        } else if (control == RESULT) {
          char *new_res = (char *)malloc(data_size);
          memcpy(new_res, ptr, data_size);
          rbuf->recv_result.push_back(new_res);
        } else if (control == NODE_RESULT) {
          char *new_res = (char *)malloc(data_size);
          memcpy(new_res, ptr, data_size);
          rbuf->recv_node_res.push_back(new_res);
        } else if (control == NODE_SYNC) {
          char *new_res = (char *)malloc(data_size);
          memcpy(new_res, ptr, data_size);
          rbuf->recv_node_sync.push_back(new_res);
        } else if (control == COMPUTE) {
          char *new_cmp = (char *)malloc(data_size);
          memcpy(new_cmp, ptr, data_size);
          rbuf->recv_compute.push_back(new_cmp);
        } else if (control == TERMINATION) {
          term[m_id] = true;
          size_t offset = 0;
          size_t q_correct{0}, q_total{0}, q_query_load{0};
          double cumu_lat_us{0.0};
          memcpy(
              &q_correct, static_cast<char *>(ptr) + offset, sizeof(q_correct));
          offset += sizeof(q_correct);
          correct += q_correct;
          memcpy(&q_total, static_cast<char *>(ptr) + offset, sizeof(q_total));
          offset += sizeof(q_total);
          total += q_total;
          memcpy(
              &q_query_load, static_cast<char *>(ptr) + offset,
              sizeof(q_query_load));
          offset += sizeof(q_query_load);
          query_load += q_query_load;

          memcpy(
              &cumu_lat_us, static_cast<char *>(ptr) + offset,
              sizeof(cumu_lat_us));
          offset += sizeof(cumu_lat_us);
          query_lat_sum += cumu_lat_us;

          size_t recv_computation_cnt;
          memcpy(
              &recv_computation_cnt, static_cast<char *>(ptr) + offset,
              sizeof(size_t));
          offset += sizeof(recv_computation_cnt);
          all_computation_cnt += recv_computation_cnt;
        } else {
          printf("Error: Unexpected buff contorl flag %d.\n", control);
        }
        post_write_recv(m_id, true_wr_id);
        if (control != CLEAR) {
          send_write_release(m_id, buf_id);
        }
      }
    }
  }
  return ne;
}

void RdmaCommunication::send_write_release(int machine_id, uint32_t buf_id) {
  auto *rbuf = recv_write_buf.getLocal();
  rbuf->release_id_list[machine_id]->push_back(buf_id);
  if (rbuf->release_id_list[machine_id]->size() >= RELEASE_BLOCK) {
    auto *sbuf = send_write_buf.getLocal();
    uint32_t num = 0;
    uint32_t send_buf_id = get_write_send_buf(machine_id, true, true);
    uint32_t *ptr = (uint32_t *)sbuf->buffer[machine_id][send_buf_id];
    while (rbuf->release_id_list[machine_id]->size() > 0) {
      uint32_t release_id = rbuf->release_id_list[machine_id]->front();
      rbuf->release_id_list[machine_id]->pop_front();
      ptr[num + 1] = release_id;
      num++;
    }
    ptr[0] = num;
    post_write_send(machine_id, send_buf_id, (num + 1) * 4, RELEASE);
  }
}

void RdmaCommunication::clear_write_release() {
  auto *rbuf = recv_write_buf.getLocal();
  for (uint32_t machine_id = 0; machine_id < MACHINE_NUM; machine_id++) {
    if (rbuf->release_id_list[machine_id]->size() > 0) {
      auto *sbuf = send_write_buf.getLocal();
      uint32_t num = 0;
      uint32_t send_buf_id = get_write_send_buf(machine_id, true, false);
      uint32_t *ptr = (uint32_t *)sbuf->buffer[machine_id][send_buf_id];
      while (rbuf->release_id_list[machine_id]->size() > 0) {
        uint32_t release_id = rbuf->release_id_list[machine_id]->front();
        rbuf->release_id_list[machine_id]->pop_front();
        ptr[num + 1] = release_id;
        num++;
      }
      ptr[0] = num;
      post_write_send(machine_id, send_buf_id, (num + 1) * 4, CLEAR);
    }
  }
}

void RdmaCommunication::com_read_init() {
  on_each([&](uint64 tid, uint64 total) {
    auto *ctx = rdma_ctx.getLocal();
    ctx->read_init(&rdma_param);
    auto *buf = read_buf.getLocal();
    for (int b = 0; b < MAX_CACHE_VEC; b++) {
      buf->buff_id_list.push_back(b);
    }
  });
}

char *RdmaCommunication::sync_read(
    int machine_id, size_t internal_id, uint32_t size) {
  std::vector<BufferCache> vecs{
      BufferCache(machine_id, internal_id, internal_id, -1, NULL)};
  post_read(vecs, size);
  if (poll_read()) {
    fprintf(stderr, "sync_read: poll failed\n");
    return nullptr;
  }
  release_cache(vecs[0].buffer_id);
  return vecs[0].buffer_ptr;
}

int RdmaCommunication::post_read(
    std::vector<BufferCache> &vectors, size_t vec_size, uint32_t query_id) {
  auto *ctx = rdma_ctx.getLocal();
  auto *buf = read_buf.getLocal();

  memset(ctx->wr_cnt, 0, sizeof(ctx->wr_cnt));
  std::vector<uint32_t> machines;
  ctx->post_num = vectors.size();
  for (BufferCache &vec : vectors) {
    uint32_t m = vec.machine_id;
    size_t offset = vec.internal_id * vec_size;
    assert(buf->buff_id_list.size() > 0);
    vec.buffer_id = buf->buff_id_list.front();
    buf->buff_id_list.pop_front();
    vec.buffer_ptr = (char *)ctx->read_to_ptr[vec.buffer_id];
    buf->bufcache_ptr[vec.buffer_id] = vec;
    size_t &r = ctx->wr_cnt[m];
    if (r >= MAX_READ_NUM) {
      fprintf(
          stderr, "Error: Exceed the max read wr buffer. n=%zu m=%u r=%zu\n",
          vectors.size(), m, r);
      abort();
    }
    ctx->sge_list[vec.buffer_id].length = vec_size;
#ifdef COMM_PROFILE
    ctx->read_size += vec_size;
    ctx->read_cnt++;
#endif
    ctx->wr[m][r].sg_list = &ctx->sge_list[vec.buffer_id];
    ctx->wr[m][r].wr.rdma.remote_addr =
        ctx->remote_mr_msg[m].read_vaddr + offset;
    ctx->wr[m][r].wr_id = (query_id << 16) | vec.buffer_id;
    ctx->wr[m][r].next = NULL;
    if (r > 0) {
      ctx->wr[m][r - 1].next = &ctx->wr[m][r];
    } else {
      machines.emplace_back(m);
    }
    r++;
  }

  for (uint32_t m : machines) {
    ctx->run_post_read(m);
  }

  return 0;
}

int RdmaCommunication::poll_read() {
  auto *ctx = rdma_ctx.getLocal();
  ctx->collect_num = 0;
  do {
    if (poll_send() < 0) {
      fprintf(stderr, "poll_read: send completion failed\n");
      return -1;
    }
  } while (ctx->collect_num < ctx->post_num);

  return 0;
}

void RdmaCommunication::release_cache(int buffer_id) {
  auto *buf = read_buf.getLocal();
  buf->buff_id_list.push_back(buffer_id);
}

char *RdmaCommunication::fetch_vector(
    uint32_t internal_id, size_t machine_id, size_t vec_size) {
  return sync_read(machine_id, internal_id, vec_size);
}

void RdmaCommunication::com_write_init() {
  on_each([&](uint64 tid, uint64 total) {
    auto *ctx = rdma_ctx.getLocal();
    auto *sbuf = send_write_buf.getLocal();
    ctx->write_init(&rdma_param);

    for (int m = 0; m < MACHINE_NUM; m++) {
      for (int buf_id = 0; buf_id < MAX_WRITE_NUM; buf_id++) {
        sbuf->buff_id_list[m]->push_back(buf_id);
      }
    }
    for (int m = 0; m < MACHINE_NUM; m++) {
      for (int buf_id = 0; buf_id < MAX_WRITE_NUM; buf_id++) {
        post_write_recv(m, buf_id);
      }
    }
  });
}

int RdmaCommunication::get_write_send_buf(
    int machine_id, bool send_release, bool pop) {
  auto *sbuf = send_write_buf.getLocal();
  if (!send_release) {
    do {
      poll_send();
      poll_recv();
    } while (sbuf->buff_id_list[machine_id]->size() <= RELEASE_BLOCK);
  } else {
    if (sbuf->buff_id_list[machine_id]->size() == 0) {
      printf("Error: expect sbuf always avaliable.\n");
      exit(0);
    }
  }
  uint32_t buf_id = sbuf->buff_id_list[machine_id]->front();
  if (pop) {
    sbuf->buff_id_list[machine_id]->pop_front();
  } else {
    sbuf->buff_id_list[machine_id]->pop_front();
    sbuf->buff_id_list[machine_id]->push_back(buf_id);
  }
  return buf_id;
}

int RdmaCommunication::post_write_send(
    int machine_id, uint32_t buf_id, uint32_t size, ControlType control) {
  auto *ctx = rdma_ctx.getLocal();

  size_t offset = MAX_QUERYBUFFER_SIZE * buf_id;
#ifdef COMM_PROFILE
  ctx->write_size += size;
  ctx->write_cnt++;
  ctx->type_write_size[control] += size;
  ctx->type_write_cnt[control]++;
#endif
  if (size > MAX_QUERYBUFFER_SIZE) {
    printf("Error: Exceed the max write buffer size.\n");
    abort();
  }
  ctx->run_post_write_send(
      machine_id, buf_id, offset, size, control << 16 | buf_id);
  return 0;
}

int RdmaCommunication::post_sg_write_send(
    int machine_id, uint32_t buf_id, std::vector<char *> &sg_ptr,
    std::vector<uint32_t> &sg_size, ControlType control) {
  auto *ctx = rdma_ctx.getLocal();
  size_t offset = MAX_QUERYBUFFER_SIZE * buf_id;
  ctx->run_post_sg_write_send(
      machine_id, buf_id, offset, sg_ptr, sg_size, control << 16 | buf_id);
  return 0;
}

int RdmaCommunication::post_write_recv(int machine_id, uint32_t buf_id) {
  auto *ctx = rdma_ctx.getLocal();
  ctx->run_post_write_recv(machine_id, buf_id);
  return 0;
}

void RdmaCommunication::send_term(
    int my_machine_id, size_t q_correct, size_t q_total,
    size_t q_query_load, double cumu_lat_us, size_t computation_cnt) {
  auto *buf = send_write_buf.getLocal();
  for (int m = 0; m < MACHINE_NUM; m++) {
    if (m == my_machine_id) {
      term[m] = true;
      correct += q_correct;
      total += q_total;
      query_load += q_query_load;
      continue;
    }
    uint32_t buf_id = get_write_send_buf(m);
    size_t offset = 0;
    char *ptr = buf->buffer[m][buf_id];
    memcpy(static_cast<char *>(ptr) + offset, &q_correct, sizeof(q_correct));
    offset += sizeof(q_correct);
    memcpy(static_cast<char *>(ptr) + offset, &q_total, sizeof(q_total));
    offset += sizeof(q_total);
    memcpy(
        static_cast<char *>(ptr) + offset, &q_query_load, sizeof(q_query_load));
    offset += sizeof(q_query_load);
    memcpy(
        static_cast<char *>(ptr) + offset, &cumu_lat_us, sizeof(cumu_lat_us));
    offset += sizeof(cumu_lat_us);
    memcpy(
        static_cast<char *>(ptr) + offset, &computation_cnt,
        sizeof(computation_cnt));
    offset += sizeof(computation_cnt);

    post_write_send(m, buf_id, offset, TERMINATION);
  }
}

bool RdmaCommunication::check_term() {
  bool all_term = true;
  for (int m = 0; m < MACHINE_NUM; m++) {
    all_term &= term[m];
  }
  return all_term;
}

void RdmaCommunication::reset_term() {
  for (int m = 0; m < MACHINE_NUM; m++) {
    term[m] = false;
  }
  correct = total = query_load = 0;
  query_lat_sum = 0.0;
  all_computation_cnt = 0;
}

void RdmaCommunication::reset_sendrecv() {
  for (int m = 0; m < MACHINE_NUM; m++) {
    auto *sbuf = send_write_buf.getLocal();
    do {
      poll_send();
      poll_recv();
      clear_write_release();
    } while (sbuf->buff_id_list[m]->size() < MAX_WRITE_NUM);

    auto *rbuf = recv_write_buf.getLocal();
    assert(rbuf->release_id_list[m]->size() == 0);
  }
}

void RdmaCommunication::post_partition_info(
    IndexParameter &index_param, uint32_t machine_id) {
  auto *buf = send_write_buf.getLocal();
  uint32_t buf_id = get_write_send_buf(machine_id);
  size_t len =
      index_param.serialize_info(buf->buffer[machine_id][buf_id], machine_id);
  post_write_send(machine_id, buf_id, len, PART_INFO);
}

void RdmaCommunication::poll_partition_info(IndexParameter &index_param) {
  auto *rbuf = recv_write_buf.getLocal();

  for (;;) {
    poll_send();
    poll_recv();

    if (rbuf->part_info_queue.size() > 0) {
      char *part_info_ptr = rbuf->part_info_queue.front();
      rbuf->part_info_queue.pop_front();
      index_param.deserialize_info(part_info_ptr);
      free(part_info_ptr);

      std::cout << "Recv partition info :"
                << index_param.local_replica_base_file << std::endl;
      break;
    }
  }
}

void RdmaCommunication::build_sync() {
  uint32_t sync_cnt = 0;
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    if (m == rdma_param.machine_id) continue;
    auto *buf = send_write_buf.getLocal();
    uint32_t buf_id = get_write_send_buf(m);
    memcpy(buf->buffer[m][buf_id], &rdma_param.machine_id, sizeof(uint32_t));
    post_write_send(m, buf_id, sizeof(uint32_t), BUILD_SYNC);
  }
  sync_cnt++;

  auto *rbuf = recv_write_buf.getLocal();
  while (sync_cnt < MACHINE_NUM) {
    poll_send();
    poll_recv();
    while (rbuf->build_sync_queue.size() > 0 && sync_cnt < MACHINE_NUM) {
      char *build_sync_ptr = rbuf->build_sync_queue.front();
      rbuf->build_sync_queue.pop_front();
      free(build_sync_ptr);
      sync_cnt++;
    }
  }
}

void RdmaCommunication::swap_scala_index_meta_info(
    uint32_t machine_id, std::vector<size_t> &out_remote_ngh_num,
    std::vector<std::vector<size_t>> &in_remote_ngh_num) {
  for (uint32_t m = 0; m < MACHINE_NUM; m++) {
    if (m == machine_id) continue;
    printf("send meta to %d\n", m);
    auto *buf = send_write_buf.getLocal();
    uint32_t buf_id = get_write_send_buf(m);
    size_t ofs = 0;
    memcpy(buf->buffer[m][buf_id] + ofs, &machine_id, sizeof(uint32_t));
    ofs += sizeof(uint32_t);
    std::copy(
        out_remote_ngh_num.begin(), out_remote_ngh_num.end(),
        (size_t *)(buf->buffer[m][buf_id] + ofs));
    ofs += out_remote_ngh_num.size() * sizeof(size_t);
    post_write_send(m, buf_id, ofs, SCALA_META);
  }

  auto *rbuf = recv_write_buf.getLocal();
  uint32_t meta_recv_cnt = 0;
  while (meta_recv_cnt < MACHINE_NUM - 1) {
    poll_send();
    poll_recv();
    while (rbuf->scala_meta_queue.size() > 0) {
      char *ptr = rbuf->scala_meta_queue.front();
      rbuf->scala_meta_queue.pop_front();
      size_t ofs = 0;
      uint32_t send_machine_id;
      memcpy(&send_machine_id, ptr + ofs, sizeof(uint32_t));
      ofs += sizeof(uint32_t);
      printf("recv meta from %d\n", send_machine_id);
      size_t in_ngh_num[MACHINE_NUM];
      memcpy(in_ngh_num, ptr + ofs, sizeof(in_ngh_num));
      ofs += sizeof(in_ngh_num);
      for (uint32_t m = 0; m < MACHINE_NUM; m++) {
        in_remote_ngh_num[send_machine_id].push_back(in_ngh_num[m]);
      }
      free(ptr);
      meta_recv_cnt++;
    }
  }
  printf("]]] swap over\n");
}

void RdmaCommunication::swap_scalagraph_remote_ngh(
    std::vector<char *> &out_remote_ngh_ptr,
    std::vector<size_t> &out_remote_ngh_num, char *recv_ptr,
    size_t *in_remote_ngh_start_ofs,
    std::vector<std::vector<size_t>> &in_remote_ngh_num) {
  for (uint32_t p = 0; p < MACHINE_NUM; p++) {
    if (p == rdma_param.machine_id) {
      for (uint32_t m = 0; m < MACHINE_NUM; m++) {
        if (p == m) continue;
        printf(
            "INFO: ngh send %d >>> %d size %llu\n", p, m,
            out_remote_ngh_num[m]);
        size_t send_cnt = 0;
        while (send_cnt < out_remote_ngh_num[m]) {
          auto *buf = send_write_buf.getLocal();
          uint32_t buf_id = get_write_send_buf(m);
          char *ptr = buf->buffer[m][buf_id];
          size_t offset = 0;
          memcpy(ptr + offset, &rdma_param.machine_id, sizeof(uint32_t));
          offset += sizeof(uint32_t);
          uint32_t max_trans_num = std::min(
              ((MAX_QUERYBUFFER_SIZE - offset - sizeof(uint32_t)) /
               sizeof(uint32_t)),
              out_remote_ngh_num[m] - send_cnt);
          memcpy(ptr + offset, &max_trans_num, sizeof(uint32_t));
          offset += sizeof(uint32_t);
          memcpy(
              ptr + offset,
              out_remote_ngh_ptr[m] + (size_t)send_cnt * sizeof(uint32_t),
              max_trans_num * sizeof(uint32_t));
          offset += max_trans_num * sizeof(uint32_t);
          post_write_send(m, buf_id, offset, SCALA_NGH);
          send_cnt += max_trans_num;
        }
        printf("INFO: ngh send %d >>> %d send %llu over\n", p, m, send_cnt);
      }
    } else {
      auto *rbuf = recv_write_buf.getLocal();
      uint32_t ngh_recv_cnt = 0;
      uint32_t expect_num = in_remote_ngh_num[p][rdma_param.machine_id];
      printf(
          "INFO: ngh recv %d <<< %d expect %llu\n", rdma_param.machine_id, p,
          expect_num);
      while (ngh_recv_cnt < expect_num) {
        poll_send();
        poll_recv();
        while (rbuf->scala_ngh_queue.size() > 0 && ngh_recv_cnt < expect_num) {
          char *ptr = rbuf->scala_ngh_queue.front();
          rbuf->scala_ngh_queue.pop_front();
          size_t ofs = 0;
          uint32_t send_machine_id;
          memcpy(&send_machine_id, ptr + ofs, sizeof(uint32_t));
          ofs += sizeof(uint32_t);
          uint32_t recv_num = 0;
          memcpy(&recv_num, ptr + ofs, sizeof(recv_num));
          ofs += sizeof(recv_num);
          memcpy(
              recv_ptr + in_remote_ngh_start_ofs[p] +
                  ngh_recv_cnt * sizeof(uint32_t),
              ptr + ofs, recv_num * sizeof(uint32_t));
          ngh_recv_cnt += recv_num;
          free(ptr);
        }
      }
      printf(
          "INFO: ngh recv %d <<< %d num %llu over\n", rdma_param.machine_id, p,
          ngh_recv_cnt);
    }
    build_sync();
    printf("--- Build sync ---\n");
  }
}
