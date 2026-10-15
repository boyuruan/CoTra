# Refactor notes

The refactor reorganized existing behavior without fixing the issues found
while reading the original code. No tests or search/index workloads were run
as part of that refactor. Follow-up fixes are marked below; the validation
boundary at the end describes the original refactor only.

## Pre-existing defects and follow-up status

1. **Fixed: top-index neighbor iteration was off by one.**
   In `include/anns/search/seed_search.h`, `search_top_index` obtains an array
   whose first word is the neighbor count. The original `d = 0 .. count-1`
   loop used `ngh_arr[d]`, treating the count as an ID and omitting the final
   neighbor. It now reads `neighbor_id = ngh_arr[d + 1]` and uses that same ID
   for prefetch, global-ID lookup, distance evaluation and candidate insertion.
   This fixes the traversal without changing the stored index format. The
   original defect came from `coro_sub_query_v5` in `scala_search.h`.

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

5. **Fixed: top-index construction now matches the selected distance/type.**
   `run_sample_and_build` in `src/index/top_index.cc` now uses
   `InnerProductSpace` for `float + mips`, `L2Space` for `float + l2`,
   `L2SpaceII` for `int8 + l2`, and `L2SpaceI` for `uint8 + l2`, matching
   the query path. The signed-int8 implementation is shared with search via
   `include/anns/space_l2_int8.h`; its distance arithmetic is unchanged.
   Unsupported top-index distance/type combinations are rejected instead of
   silently falling back to L2. Previously built `float + mips` and
   `int8 + l2` top indexes must be rebuilt to correct their graph topology;
   the stored index format is unchanged.

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
