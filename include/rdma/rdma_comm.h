#pragma once
#include "anns/vec_buffer.h"
#include "coromem/include/backend.h"
#include "coromem/include/galois/loops.h"
#include "index/index_param.h"
#include "rdma_context.h"

class RdmaCommunication {
 public:
  PerThreadStorage<RdmaContext> rdma_ctx;
  PerThreadStorage<VectorBuffer> read_buf;

  PerThreadStorage<SendWriteBuffer> send_write_buf;
  PerThreadStorage<RecvWriteBuffer> recv_write_buf;

  RdmaParameter rdma_param;

  // global query batch info.
  size_t vector_size;
  size_t correct{0}, total{0}, query_load{0};
  double query_lat_sum{0.0};
  // for comp eff profiling.
  size_t all_computation_cnt{0};

  RdmaCommunication() {}
  ~RdmaCommunication();

  void *shm_peer_base[MACHINE_NUM]{nullptr};
  size_t shm_peer_size[MACHINE_NUM]{0};
  std::string shm_my_path;
  void com_init_shm(char *vec_part_ptr, size_t vec_part_len);
  void shm_cleanup();

  // Indicator for termination.
  bool term[MACHINE_NUM];

  int ctx_close_connection();

  void com_init(
      char *vec_part_ptr, size_t vec_part_len, RdmaParameter &rdma_param);

  /**
   * Poll cq. Include send cq and recv cq.
   */
  int poll_send();
  int poll_recv();

  /**
   * Read operation. Include sync read and async read.
   */
  void com_read_init();
  char *sync_read(int machine_id, size_t internal_id, uint32_t size);
  int post_read(
      std::vector<BufferCache> &vectors, size_t vec_size,
      uint32_t query_id = 0);
  int poll_read();
  void release_cache(int buffer_id);
  char *fetch_vector(uint32_t internal_id, size_t machine_id, size_t vec_size);

  /**
   * Write operation. Include sync write and async write.
   */
  void com_write_init();
  int get_write_send_buf(
      int machine_id, bool send_release = false, bool pop = true);
  int post_write_send(
      int machine_id, uint32_t buf_id, uint32_t size, ControlType control);
  int post_write_recv(int machine_id, uint32_t buf_id);
  int post_sg_write_send(
      int machine_id, uint32_t buf_id, std::vector<char *> &sg_ptr,
      std::vector<uint32_t> &size, ControlType control);
  void send_write_release(int machine_id, uint32_t buf_id);
  void clear_write_release();

  void com_send_init() {}

  /**
   * Used for termination.
   */
  void send_term(
      int my_machine_id, size_t q_correct, size_t q_total, size_t q_query_load,
      double cumu_lat_us = 0.0, size_t computation_cnt = 0);
  bool check_term();
  void reset_term();
  void reset_sendrecv();

  /* This part is for index building. */
  void post_partition_info(IndexParameter &index_param, uint32_t machine_id);
  void poll_partition_info(IndexParameter &index_param);
  void build_sync();

  // For ScalaGraph.
  void swap_scala_index_meta_info(
      uint32_t machine_id, std::vector<size_t> &out_remote_ngh_num,
      std::vector<std::vector<size_t>> &in_remote_ngh_num);

  void swap_scalagraph_remote_ngh(
      std::vector<char *> &out_remote_ngh_ptr,
      std::vector<size_t> &out_remote_ngh_num, char *recv_ptr,
      size_t *in_remote_ngh_start_ofs,
      std::vector<std::vector<size_t>> &in_remote_ngh_num);
};
