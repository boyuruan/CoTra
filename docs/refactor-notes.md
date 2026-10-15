# Refactor notes

This change reorganizes existing behavior. The issues below were found by
reading the original code and are intentionally not fixed. Their measured
impact is unknown; no tests or search/index workloads were run for the refactor.

## Suspected pre-existing defects

1. **Top-index neighbor iteration appears off by one.**
   In `include/anns/search/seed_search.h`, `search_top_index` obtains an array
   whose first word is the neighbor count, loops `d = 0 .. count-1`, and uses
   `ngh_arr[d]` as the neighbor ID. This appears to treat the count as an ID and
   omit the final neighbor. The look-ahead uses `ngh_arr[d + 1]`, but the actual
   vector lookup and distance computation still use the original `d` access.
   This came from `coro_sub_query_v5` in `scala_search.h`.

2. **Neighbor prefetch look-ahead can read past the logical list.**
   `process_inline_neighbor_tasks` in
   `include/anns/search/candidate_processing.h` reads `local_micro_task[i + 1]`
   even for the final element. Similar `ngh_arr[d + 1]` accesses occur while
   processing packed neighbor lists in candidate processing and event handlers.
   A non-faulting prefetch instruction does not make the C++ array access used
   to construct its address valid. The accesses are unchanged.

3. **Graph-layout diagnostics read an uninitialized size.**
   In `src/index/build_graph_layout.cc`, `expect_data_size` is assigned from
   `scala_data_size` before that variable receives its actual value. The value
   is printed in the expected-size diagnostic. The later size computation and
   encoder were kept in their original order.

4. **The intermediate entry-point read fills only part of a wider object.**
   `encode_partitioned_graph` declares `uint64_t read_enterpoint_node_` and reads
   only `sizeof(uint32_t)` bytes into it before evaluating it. The high bytes
   remain uninitialized, although the next assignment narrows to a 32-bit
   medoid. The declaration/read pair is retained for a separate review.

5. **Top-index construction does not follow every selected distance/type.**
   `run_sample_and_build` in `src/index/top_index.cc` constructs an L2 space for
   float data even when `distType` requests inner product. It also uses
   `L2SpaceI` for both signed and unsigned 8-bit data, while the query entry
   selects different spaces for those types. These paths were not changed.

## Refactor validation boundary

Only source comparison, formatting/diff inspection and compilation are in scope.
No unit tests, smoke checks, synthetic data generators, index builds, PQ
training, searches or benchmarks are executed. Compiling executables whose
source lives under `tests/` does not execute them.

Source review focuses on the existing index encoders, message encoders,
coroutine suspension points, queue ordering, candidate acceptance rules, seed
ownership, termination counters, batching thresholds and accounting updates.
Compilation can establish template instantiation and linking, not recall or
communication equivalence. Existing undefined behavior also prevents a general
claim of bit-for-bit runtime equivalence without a separate investigation.

The refactored `scala_index`, `scala_anns` and `pq_encode` targets compiled and
linked successfully in `cotra-softroce` with `COTRA_WITH_INDEX=ON` and
`MACHINE_NUM=4`. None of the resulting executables was run. Source comparisons
covered the moved build stages and encoders, query codecs, candidate rules,
coroutine control flow, event handlers, synchronization, CLI parsing and recall
driver, allowing for the documented extraction and identifier changes.
