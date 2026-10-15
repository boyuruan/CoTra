#pragma once

#include "anns/core_selection.h"
#include "anns/query_state.h"

// QUERY bytes and legacy serializers. Member order and widths are unchanged.
template <typename dist_t>
struct QueryWireCodec {
  /**
   * scala_v3 QUERY wire formats. A one-byte kind leads every payload so the
   * two layouts stay apart; the receiver still tells a fork from an end by
   * is_new, as it always did.
   *
   *   QW_FORK (issuer to a core machine):
   *     kind u8, qid u32, ef u32, origin u8, leader u8,
   *     ncore u8, core ids u8[], lowerBound, vector_size u32, vector,
   *     n_top u32, (dist, id) pairs.
   *   QW_END (leader back to the issuer):
   *     kind u8, qid u32, n_res u32, (dist, label u64) entries.
   *
   * The fork carries no candidate section: the issuer split its top list
   * by machine, and the receiver redoes that split (decode_into).
   * The end carries only what proc_res reads; the receiver sets state END.
   */
  enum WireKind : uint8_t { QW_FORK = 0, QW_END = 1 };

  static uint8_t wire_kind(const void* src) {
    return *static_cast<const uint8_t*>(src);
  }
  static uint32_t wire_qid(const void* src) {
    uint32_t qid;
    memcpy(&qid, static_cast<const char*>(src) + 1, sizeof(qid));
    return qid;
  }

  // Byte accounting for a serialized query. sec[0] is everything before the
  // top candidates, sec[1] the top candidates, sec[2] the candidate set,
  // sec[3] the result, each including its own count field.
  static void add_sections(
      uint64_t* sec, size_t head_end, size_t top_end, size_t cand_end,
      size_t res_end) {
    if (sec == nullptr) return;
    sec[0] += head_end;
    sec[1] += top_end - head_end;
    sec[2] += cand_end - top_end;
    sec[3] += res_end - cand_end;
  }

  static void check_wire_size(size_t offset) {
    if (offset > MAX_QUERYBUFFER_SIZE) {
      printf("Error: exceed buffer size.\n");
      abort();
    }
  }

  /**
   * Write the QW_END payload. Consumes result, as the migration always did.
   */
  static size_t encode_end(
      QueryState<dist_t>& query, void* dest, uint64_t* sec = nullptr) {
    char* base = static_cast<char*>(dest);
    char* p = base;
    wire::put<uint8_t>(p, QW_END);
    wire::put<uint32_t>(p, query.query_id);
    size_t head_end = p - base;

    wire::put<uint32_t>(p, query.result.size());
    while (!query.result.empty()) {
      const std::pair<dist_t, size_t>& elem = query.result.top();
      wire::put<dist_t>(p, elem.first);
      wire::put<uint64_t>(p, elem.second);
      query.result.pop();
    }
    size_t offset = p - base;
    add_sections(sec, head_end, head_end, head_end, offset);

#ifdef DEBUG
    memcpy(p, query.tmp_m_cnt, sizeof(query.tmp_m_cnt));
    offset += sizeof(query.tmp_m_cnt);
#endif
    check_wire_size(offset);
    return offset;
  }

  /**
   * Write the QW_FORK payload. The same bytes go to every core machine.
   */
  static size_t encode_fork(
      const QueryState<dist_t>& query, void* dest, uint64_t* sec = nullptr) {
    char* base = static_cast<char*>(dest);
    char* p = base;
    wire::put<uint8_t>(p, QW_FORK);
    wire::put<uint32_t>(p, query.query_id);
    wire::put<uint32_t>(p, query.ef);
    wire::put<uint8_t>(p, query.origin_machine);
    wire::put<uint8_t>(p, query.leader_machine);
    wire::put<uint8_t>(p, query.core_machine.size());
    for (uint32_t m : query.core_machine) {
      wire::put<uint8_t>(p, m);
    }
    wire::put<dist_t>(p, query.lowerBound);
    wire::put<uint32_t>(p, query.vector_size);
    memcpy(p, query.vector, query.vector_size);
    p += query.vector_size;
    size_t head_end = p - base;

    wire::put<uint32_t>(p, query.top_candidates.size());
    auto top_copy = query.top_candidates;
    while (!top_copy.empty()) {
      wire::put(p, top_copy.top());
      top_copy.pop();
    }
    size_t offset = p - base;
    add_sections(sec, head_end, offset, offset, offset);
    check_wire_size(offset);
    return offset;
  }

  /**
   * Apply a QUERY payload to this (pre-allocated) query. local_id is the
   * receiving machine; machine_of maps a global id to the machine that
   * holds it, the same range lookup the issuer used to split its seeds.
   */
  template <typename MachineOf>
  static void decode_into(
      QueryState<dist_t>& query, const void* src, uint32_t local_id,
      MachineOf&& machine_of) {
    const char* p = static_cast<const char*>(src);
    uint8_t kind;
    wire::get(p, kind);
    uint32_t qid;
    wire::get(p, qid);
    query.query_id = qid;

    if (kind == QW_END) {
      query.state = END;
      uint32_t n_res;
      wire::get(p, n_res);
      while (!query.result.empty()) {
        query.result.pop();
      }
      for (uint32_t i = 0; i < n_res; ++i) {
        dist_t dist;
        uint64_t label;
        wire::get(p, dist);
        wire::get(p, label);
        query.result.emplace(dist, label);
      }
#ifdef DEBUG
      memcpy(query.tmp_m_cnt, p, sizeof(query.tmp_m_cnt));
#endif
      return;
    }
    if (kind != QW_FORK) {
      printf("Error: unknown QUERY wire kind %u\n", kind);
      abort();
    }

    query.state = PRE_STAGE;
    uint32_t ef32;
    wire::get(p, ef32);
    query.ef = ef32;
    uint8_t origin8, leader8, ncore;
    wire::get(p, origin8);
    wire::get(p, leader8);
    wire::get(p, ncore);
    query.origin_machine = origin8;
    query.leader_machine = leader8;
    query.core_machine.clear();
    memset(query.is_core_machine, 0, sizeof(query.is_core_machine));
    for (uint32_t i = 0; i < ncore; ++i) {
      uint8_t m;
      wire::get(p, m);
      query.core_machine.emplace_back(m);
      query.is_core_machine[m] = 1;
    }
    wire::get(p, query.lowerBound);
    uint32_t vsize;
    wire::get(p, vsize);
    query.vector_size = vsize;
    if (query.vector == nullptr)
      query.vector = (char*)malloc(query.vector_size);
    memcpy(query.vector, p, query.vector_size);
    p += query.vector_size;

    // The issuer gave each core machine the top entries that live on it,
    // and the leader also every entry that lives on a non-core machine.
    // Rebuild that seed set from the top list instead of carrying it.
    uint32_t n_top;
    wire::get(p, n_top);
    while (!query.top_candidates.empty()) {
      query.top_candidates.pop();
    }
    while (!query.candidate_set.empty()) {
      query.candidate_set.pop();
    }
    for (uint32_t i = 0; i < n_top; ++i) {
      std::pair<dist_t, uint32_t> elem;
      wire::get(p, elem);
      query.top_candidates.push(elem);
      uint32_t mid = machine_of(elem.second);
      if (cotra::seed_target(
              mid, query.leader_machine, query.is_core_machine) == local_id) {
        query.candidate_set.emplace(-elem.first, elem.second);
        query.visit_hash.CheckAndSet(elem.second);
      }
    }
  }

  // Serialization function
  static void encode_legacy(const QueryState<dist_t>& query, void* dest) {
    size_t offset = 0;
    memcpy(static_cast<char*>(dest) + offset, &query.ef, sizeof(query.ef));
    offset += sizeof(query.ef);
    memcpy(
        static_cast<char*>(dest) + offset, &query.query_id,
        sizeof(query.query_id));
    offset += sizeof(query.query_id);
    memcpy(
        static_cast<char*>(dest) + offset, &query.origin_machine,
        sizeof(query.origin_machine));
    offset += sizeof(query.origin_machine);

    memcpy(
        static_cast<char*>(dest) + offset, &query.current_node_id,
        sizeof(query.current_node_id));
    offset += sizeof(query.current_node_id);
    memcpy(
        static_cast<char*>(dest) + offset, &query.lowerBound,
        sizeof(query.lowerBound));
    offset += sizeof(query.lowerBound);
    memcpy(
        static_cast<char*>(dest) + offset, &query.vector_size,
        sizeof(query.vector_size));
    offset += sizeof(query.vector_size);
    memcpy(static_cast<char*>(dest) + offset, query.vector, query.vector_size);
    offset += query.vector_size;
    memcpy(
        static_cast<char*>(dest) + offset, &query.state, sizeof(query.state));
    offset += sizeof(query.state);

    // Serialize visit_hash
    // #ifdef MSGQ
    //     memcpy(
    //         static_cast<char*>(dest) + offset, &visit_hash.m_secondHash,
    //         sizeof(visit_hash.m_secondHash));
    //     offset += sizeof(visit_hash.m_secondHash);
    //     memcpy(
    //         static_cast<char*>(dest) + offset, &visit_hash.m_exp,
    //         sizeof(visit_hash.m_exp));
    //     offset += sizeof(visit_hash.m_exp);
    //     memcpy(
    //         static_cast<char*>(dest) + offset, &visit_hash.m_poolSize,
    //         sizeof(visit_hash.m_poolSize));
    //     offset += sizeof(visit_hash.m_poolSize);

    //     size_t hash_table_size = (visit_hash.m_poolSize + 1) * 2;
    //     // printf("hash_table_size %d offset %d\n", hash_table_size, offset);
    //     memcpy(
    //         static_cast<char*>(dest) + offset, visit_hash.m_hashTable.get(),
    //         sizeof(OptHashPosVector::SizeType) * hash_table_size);
    //     offset += sizeof(OptHashPosVector::SizeType) * hash_table_size;

#ifdef PROFILER
    query.profiler.serialize(static_cast<char*>(dest), offset);
#endif

    // Serialize top_candidates
    // printf("Serialize top_candidates\n");
    size_t top_candidates_size = query.top_candidates.size();
    memcpy(
        static_cast<char*>(dest) + offset, &top_candidates_size,
        sizeof(top_candidates_size));
    offset += sizeof(top_candidates_size);

    auto copy_queue = query.top_candidates;
    while (!copy_queue.empty()) {
      auto& elem = copy_queue.top();
      memcpy(static_cast<char*>(dest) + offset, &elem, sizeof(elem));
      offset += sizeof(elem);
      copy_queue.pop();
    }

    // Serialize candidate_set
    size_t candidate_set_size = query.candidate_set.size();
    memcpy(
        static_cast<char*>(dest) + offset, &candidate_set_size,
        sizeof(candidate_set_size));
    offset += sizeof(candidate_set_size);

    copy_queue = query.candidate_set;
    while (!copy_queue.empty()) {
      auto& elem = copy_queue.top();
      memcpy(static_cast<char*>(dest) + offset, &elem, sizeof(elem));
      offset += sizeof(elem);
      copy_queue.pop();
    }

    // Serialize result
    size_t result_size = query.result.size();
    memcpy(
        static_cast<char*>(dest) + offset, &result_size, sizeof(result_size));
    offset += sizeof(result_size);

    auto copy_result = query.result;
    while (!copy_result.empty()) {
      auto& elem = copy_result.top();
      memcpy(static_cast<char*>(dest) + offset, &elem, sizeof(elem));
      offset += sizeof(elem);
      copy_result.pop();
    }

    assert(offset <= MAX_QUERYBUFFER_SIZE);
  }

  // Deserialization function
  /*
    Construct QueryState from given pointer.
  */
  static QueryState<dist_t>* decode_legacy(const void* src) {
    QueryState<dist_t>* msg = new QueryState<dist_t>();
    size_t offset = 0;
    memcpy(&msg->ef, static_cast<const char*>(src) + offset, sizeof(msg->ef));
    offset += sizeof(msg->ef);
    memcpy(
        &msg->query_id, static_cast<const char*>(src) + offset,
        sizeof(msg->query_id));
    offset += sizeof(msg->query_id);

    memcpy(
        &msg->origin_machine, static_cast<const char*>(src) + offset,
        sizeof(msg->origin_machine));
    offset += sizeof(msg->origin_machine);
    memcpy(
        &msg->current_node_id, static_cast<const char*>(src) + offset,
        sizeof(msg->current_node_id));
    offset += sizeof(msg->current_node_id);

    memcpy(
        &msg->lowerBound, static_cast<const char*>(src) + offset,
        sizeof(msg->lowerBound));
    offset += sizeof(msg->lowerBound);
    memcpy(
        &msg->vector_size, static_cast<const char*>(src) + offset,
        sizeof(msg->vector_size));
    offset += sizeof(msg->vector_size);
    if (msg->vector == nullptr) msg->vector = (char*)malloc(msg->vector_size);
    memcpy(
        msg->vector, static_cast<const char*>(src) + offset, msg->vector_size);
    offset += msg->vector_size;
    memcpy(
        &msg->state, static_cast<const char*>(src) + offset,
        sizeof(msg->state));
    offset += sizeof(msg->state);

    // #ifdef MSGQ
    //     memcpy(
    //         &msg->visit_hash.m_secondHash, static_cast<const char*>(src) +
    //         offset, sizeof(msg->visit_hash.m_secondHash));
    //     offset += sizeof(msg->visit_hash.m_secondHash);
    //     memcpy(
    //         &msg->visit_hash.m_exp, static_cast<const char*>(src) + offset,
    //         sizeof(msg->visit_hash.m_exp));
    //     offset += sizeof(msg->visit_hash.m_exp);
    //     memcpy(
    //         &msg->visit_hash.m_poolSize, static_cast<const char*>(src) +
    //         offset, sizeof(msg->visit_hash.m_poolSize));
    //     offset += sizeof(msg->visit_hash.m_poolSize);

    //     size_t hash_table_size = (msg->visit_hash.m_poolSize + 1) * 2;
    //     msg->visit_hash.m_hashTable.reset(
    //         new OptHashPosVector::SizeType[hash_table_size]);
    //     memcpy(
    //         msg->visit_hash.m_hashTable.get(),
    //         static_cast<const char*>(src) + offset,
    //         sizeof(OptHashPosVector::SizeType) * hash_table_size);
    //     offset += sizeof(OptHashPosVector::SizeType) * hash_table_size;

    // #ifdef PROFILER
    //     msg->profiler.deserialize(static_cast<const char*>(src), offset);
    // #endif

    // Deserialize top_candidates
    size_t top_candidates_size;
    memcpy(
        &top_candidates_size, static_cast<const char*>(src) + offset,
        sizeof(top_candidates_size));
    offset += sizeof(top_candidates_size);

    for (size_t i = 0; i < top_candidates_size; ++i) {
      std::pair<dist_t, uint32_t> elem;
      memcpy(&elem, static_cast<const char*>(src) + offset, sizeof(elem));
      offset += sizeof(elem);
      msg->top_candidates.push(elem);
    }

    // Deserialize candidate_set
    size_t candidate_set_size;
    memcpy(
        &candidate_set_size, static_cast<const char*>(src) + offset,
        sizeof(candidate_set_size));
    offset += sizeof(candidate_set_size);

    for (size_t i = 0; i < candidate_set_size; ++i) {
      std::pair<dist_t, uint32_t> elem;
      memcpy(&elem, static_cast<const char*>(src) + offset, sizeof(elem));
      offset += sizeof(elem);
      msg->candidate_set.push(elem);
    }

    // Deserialize result
    size_t result_size;
    memcpy(
        &result_size, static_cast<const char*>(src) + offset,
        sizeof(result_size));
    offset += sizeof(result_size);

    for (size_t i = 0; i < result_size; ++i) {
      std::pair<dist_t, size_t> elem;
      memcpy(&elem, static_cast<const char*>(src) + offset, sizeof(elem));
      offset += sizeof(elem);
      msg->result.push(elem);
    }

    // printf("offset: %d\n", offset);

    return msg;
  }
};
