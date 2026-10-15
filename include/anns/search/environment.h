#pragma once

// Template implementation for ScalaSearch; included after its declaration.
namespace hnswlib {

template <typename dist_t>
void ScalaSearch<dist_t>::init_pq_gate() {
  ScalaGraphIndex *g = graph_index.scala_graph;
  pq_codes.load(
      anns_param.pq_prefix, anns_param.data_type_str, g->kmeans_pos_label_map,
      g->all_vec_num);
  if (pq_codes.dim != anns_param.vecdim) {
    fprintf(
        stderr, "PQ dim %u does not match data dim %zu\n", pq_codes.dim,
        anns_param.vecdim);
    abort();
  }
  // A local vector should be far closer to its own code than to another
  // vector's. If not, the codes are not in this graph's pos order.
  std::mt19937 rng(1);
  double own = 0, other = 0;
  uint32_t n = std::min<uint32_t>(1000, g->local_part_vec_num);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t local = rng() % g->local_part_vec_num;
    const char *vec = g->vector_ptr + (size_t)local * data_label_size;
    own += pq_codes.dist(vec, g->partition_ofs + local);
    other += pq_codes.dist(vec, rng() % g->all_vec_num);
  }
  printf(
      "PQ self-check over %u local vectors: own code %.1f, random code "
      "%.1f\n",
      n, own / n, other / n);
  if (own >= 0.5 * other) {
    fprintf(stderr, "PQ codes do not match the graph's vector order\n");
    abort();
  }
  pq_gate.codes = &pq_codes;
  pq_gate.shadow = anns_param.pq_tau < 0;
  pq_gate.scale = 1.0f + std::max(0.0f, anns_param.pq_tau);
  printf(
      "PQ gate on inline remote neighbors: tau %.3f%s\n", anns_param.pq_tau,
      pq_gate.shadow ? " (shadow: count only)" : "");
}

template <typename dist_t>
void ScalaSearch<dist_t>::run_rdma() {
  partition_ptr = graph_index.get_partition_ptr();
  partition_size = graph_index.get_partition_size();
  rdma_comm.com_init(partition_ptr, partition_size, rdma_param);
  rdma_comm.com_read_init();
  rdma_comm.com_write_init();
}

template <typename dist_t>
void ScalaSearch<dist_t>::end_rdma() {
  if (rdma_comm.ctx_close_connection()) {
    fprintf(stderr, "Failed to close connection between server and client\n");
    return;
  }
}

template <typename dist_t>
void ScalaSearch<dist_t>::initialize_batch(
    const void *query_data, uint32_t vec_size, uint32_t query_num) {
#ifdef PROF_DEGREE
  memset(local_distri, 0, sizeof(local_distri));
  memset(remote_distri, 0, sizeof(remote_distri));
  local_cnt = local_deg = remote_cnt = remote_deg = 0;
#endif

  for (uint32_t q = 0; q < query_num; q++) {
    // Previous ef's objects stay until here. Recall accounting does not
    // delete them: a second completion would free the same pointer twice.
    delete global_query[q];
    global_query[q] = new QueryState<dist_t>(
        (char *)(query_data + (uint64_t)q * vec_size), vec_size, q, ef_,
        rdma_param.machine_id);
  }
  on_each([&](uint64 tid, uint64 total) {
#ifdef PROFILER
    auto *local_prof = all_profiler.getLocal();
    Profiler &m_profiler = *local_prof;
    m_profiler.clear();
#endif
    // init comm size count.
    auto *ctx = rdma_comm.rdma_ctx.getLocal();
#ifdef COMM_PROFILE
    ctx->clear_size_cnt();
#endif
    auto *tman = task_ma.getLocal();
    tman->clear();

    delete[] tman->global_query;
    tman->global_query =
        (QueryState<dist_t> **)new QueryState<dist_t> *[anns_param.qsize];
    for (uint32_t q = 0; q < query_num; q++) {
      // Init query.
      tman->global_query[q] = global_query[q];
    }

    tman->ba_task_queue = &ba_task_queue;
    if (graph_index.scala_graph != nullptr) {
      tman->machine_of = [this](uint32_t id) {
        return graph_index.scala_graph->get_machine_id(id);
      };
    }
  });
}

}  // namespace hnswlib
