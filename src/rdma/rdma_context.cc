#include "rdma/rdma_context.h"

#include <cstring>
#include <thread>

#include "anns/vec_buffer.h"
#include "coromem/include/backend.h"

void RdmaContext::shm_push_send_wc(uint64_t wr_id, uint32_t opcode) {
  ShmWcEntry &e = shm_my_thread->send_cq[shm_send_tail % SHM_SEND_CQ_CAP];
  e.wr_id = wr_id;
  e.byte_len = 0;
  e.imm_data = 0;
  e.opcode = opcode;
  e.status = IBV_WC_SUCCESS;
  shm_send_tail++;
}

void RdmaContext::shm_push_recv_wc(
    int target_machine, uint64_t wr_id, uint32_t byte_len, uint32_t imm_data,
    uint32_t opcode) {
  ShmSpscRing &ring =
      shm_peer_thread[target_machine]->recv_rings[this->machine_id];
  uint64_t w = ring.write_idx.load(std::memory_order_relaxed);
  while (w - ring.read_idx.load(std::memory_order_acquire) >= SHM_CQ_CAP) {
    std::this_thread::yield();
  }
  ShmWcEntry &e = ring.entries[w & (SHM_CQ_CAP - 1)];
  e.wr_id = wr_id;
  e.byte_len = byte_len;
  e.imm_data = imm_data;
  e.opcode = opcode;
  e.status = IBV_WC_SUCCESS;
  ring.write_idx.store(w + 1, std::memory_order_release);
}

void RdmaContext::read_init(RdmaParameter *param) {
  for (int m = 0; m < MACHINE_NUM; m++) {
    for (int r = 0; r < MAX_READ_NUM; r++) {
      memset(&wr[m][r], 0, sizeof(ibv_send_wr));
      wr[m][r].wr.rdma.remote_addr = remote_mr_msg[m].read_vaddr;
      wr[m][r].wr.rdma.rkey = remote_mr_msg[m].read_rkey;
      wr[m][r].num_sge = MAX_SEND_SGE;
      wr[m][r].next = NULL;
      wr[m][r].send_flags = IBV_SEND_SIGNALED;
      wr[m][r].opcode = IBV_WR_RDMA_READ;
    }
  }

  for (int b = 0; b < MAX_CACHE_VEC; b++) {
    sge_list[b].addr = (uintptr_t)read_to_ptr[b];
    sge_list[b].lkey = 0;
  }
}

int RdmaContext::run_post_read(int machine_id) {
  for (struct ibv_send_wr *w = &wr[machine_id][0]; w != nullptr;
       w = w->next) {
    memcpy(
        (void *)w->sg_list->addr, (const void *)w->wr.rdma.remote_addr,
        w->sg_list->length);
    shm_push_send_wc(w->wr_id, IBV_WC_RDMA_READ);
  }
  return 0;
}

void RdmaContext::write_init(RdmaParameter *param) {
  for (int m = 0; m < param->machine_num; m++) {
    for (int i = 0; i < MAX_WRITE_NUM; i++) {
      memset(&wwr[m][i], 0, sizeof(struct ibv_send_wr));
      wwr[m][i].wr.rdma.rkey = remote_mr_msg[m].write_rkey;
      write_sge_list[m][i][0].addr = (uintptr_t)send_write_ptr[m][i];
      write_sge_list[m][i][0].lkey = 0;
      wwr[m][i].sg_list = &write_sge_list[m][i][0];
      wwr[m][i].num_sge = MAX_SEND_SGE;
      wwr[m][i].wr_id = (m << 16) | i;
      wwr[m][i].next = NULL;
      wwr[m][i].send_flags = IBV_SEND_SIGNALED;
      wwr[m][i].opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    }

    for (int i = 0; i < MAX_WRITE_NUM; i++) {
      memset(&rwwr[m][i], 0, sizeof(struct ibv_recv_wr));
      rwwr[m][i].sg_list = NULL;
      rwwr[m][i].num_sge = 0;
      rwwr[m][i].wr_id = (m << 16) | i;
      rwwr[m][i].next = NULL;
    }
  }
}

int RdmaContext::run_post_write_send(
    int machine_id, uint32_t buf_id, size_t offset, uint32_t size,
    uint32_t control) {
  char *dst =
      shm_peer_thread[machine_id]->recv_buf[this->machine_id][0] + offset;
  memcpy(dst, send_write_ptr[machine_id][buf_id], size);
  shm_push_recv_wc(
      machine_id, (uint64_t(this->machine_id) << 16) | buf_id, size,
      htonl(control), IBV_WC_RECV_RDMA_WITH_IMM);
  return 0;
}

int RdmaContext::run_post_sg_write_send(
    int machine_id, uint32_t buf_id, size_t offset, std::vector<char *> &sg_ptr,
    std::vector<uint32_t> &sg_size, uint32_t control) {
  int sg_num = sg_ptr.size();
  if (sg_num > MAX_SGE_NUM) {
    printf("Exceed max sge :%u\n", sg_num);
    abort();
  }
  char *dst =
      shm_peer_thread[machine_id]->recv_buf[this->machine_id][0] + offset;
  uint32_t total_size = 0;
  if (sg_num > 0) {
    memcpy(dst, send_write_ptr[machine_id][buf_id], sg_size[0]);
    total_size += sg_size[0];
    for (int s = 1; s < sg_num; s++) {
      memcpy(dst + total_size, sg_ptr[s], sg_size[s]);
      total_size += sg_size[s];
    }
  }
  shm_push_recv_wc(
      machine_id, (uint64_t(this->machine_id) << 16) | buf_id, total_size,
      htonl(control), IBV_WC_RECV_RDMA_WITH_IMM);
  return 0;
}

int RdmaContext::run_post_write_recv(int machine_id, size_t buff_id) {
  return 0;
}

int RdmaContext::poll_SEND() {
  int count = 0;
  while (shm_send_head < shm_send_tail && count < MAX_WC_NUM) {
    const ShmWcEntry &e =
        shm_my_thread->send_cq[shm_send_head % SHM_SEND_CQ_CAP];
    s_wc[count].wr_id = e.wr_id;
    s_wc[count].status = (enum ibv_wc_status)e.status;
    s_wc[count].opcode = (enum ibv_wc_opcode)e.opcode;
    s_wc[count].byte_len = e.byte_len;
    s_wc[count].imm_data = e.imm_data;
    s_wc[count].vendor_err = 0;
    shm_send_head++;
    count++;
  }
  if (shm_send_head == shm_send_tail) {
    shm_send_head = shm_send_tail = 0;
  }
  return count;
}

int RdmaContext::poll_RECV() {
  int count = 0;
  bool progress = true;
  while (progress && count < MAX_WC_NUM) {
    progress = false;
    for (int m = 0; m < machine_num && count < MAX_WC_NUM; m++) {
      uint32_t src = (shm_rr_peer + m) % machine_num;
      if (src == (uint32_t)machine_id) continue;
      ShmSpscRing &ring = shm_my_thread->recv_rings[src];
      uint64_t r = ring.read_idx.load(std::memory_order_relaxed);
      uint64_t w = ring.write_idx.load(std::memory_order_acquire);
      if (r < w) {
        const ShmWcEntry &e = ring.entries[r & (SHM_CQ_CAP - 1)];
        r_wc[count].wr_id = e.wr_id;
        r_wc[count].status = (enum ibv_wc_status)e.status;
        r_wc[count].opcode = (enum ibv_wc_opcode)e.opcode;
        r_wc[count].byte_len = e.byte_len;
        r_wc[count].imm_data = e.imm_data;
        r_wc[count].vendor_err = 0;
        ring.read_idx.store(r + 1, std::memory_order_release);
        count++;
        progress = true;
      }
    }
    shm_rr_peer = (shm_rr_peer + 1) % machine_num;
  }
  return count;
}
