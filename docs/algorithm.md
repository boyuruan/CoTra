# Algorithm and module guide

The source is organized around the build pipeline and the per-query coroutine.
Stage functions use the existing state and queues; they do not introduce a new
scheduler, a synchronous barrier between search stages, or a new wire format.

## Index construction

`tests/scala_index.cc` parses the arguments with the thread pool paused, then
calls `ScalaBuild::run` in `src/index/scala_build.cc`.

| Order | Stage | Implementation | Produced or consumed data |
| --- | --- | --- | --- |
| 1 | Initialize transport | `scala_build.cc` | Shared-memory communication buffers |
| 2 | Partition vectors | `build_partition.cc` | Balanced k-means ownership and two-replica build partitions |
| 3 | Distribute metadata | `build_partition.cc` | Point count and dimension for each process |
| 4 | Build local graphs | `build_local_graph.cc`, `disk_utils.cc` | Vamana graphs on replica partitions, with degree `2R/3` |
| 5 | Resume the thread pool | `scala_build.cc` | Same handoff point as before the refactor |
| 6 | Write and load maps | `build_partition.cc` | Original label ↔ graph position; unique partition ownership |
| 7 | Merge replica graphs | `build_replica_merge.cc`, `disk_utils.cc` | Neighbor lists collected at each owner, deduplicated, shuffled and truncated to `R` |
| 8 | Encode intermediate graph | `build_intermediate_graph.cc` | Existing `_final_b1.index` representation, with graph IDs and original labels |
| 9 | Encode CoTra layout | `build_graph_layout.cc` | Per-partition graph/data files and exchanged out-of-line neighbor lists |
| 10 | Build top index on machine 0 | `top_index.cc` | Sampled HNSW data loaded by every search process |

The intermediate B1-named file is still part of the CoTra build pipeline. It is
not a second search algorithm. The two replicas are a construction device;
final base vectors have unique partition ownership.
`merge_replica_graphs` writes the intermediate representation before its
existing build barrier; the CoTra layout encoder runs after that barrier.

`IndexParameter` holds configuration, dataset metadata and filenames.
`IndexParameter::workspace` is a `BuildWorkspace` containing mutable maps,
replica IDs, graph-exchange buffers and completion counters. Their initialization
and release remain in the original stages. Build helpers continue to take the
same parameter object so moving state does not require a second copy.

## Storage and IDs

`ScalaGraphIndex` loads the existing files and exposes vector and adjacency
access. `PartitionMapView` is a non-owning view of its partition boundaries and
original-label map. It distinguishes:

- Original label: the ID in the dataset and ground truth.
- Graph ID: the global position after grouping points by partition.
- Local ID: graph ID minus the start of its partition.
- Top-index ID: local position in the replicated top-index file; its stored
  label maps to a graph ID.

CoTra adjacency is grouped by the neighbors' destination partition. Local IDs
are inline. A remote group of at most `INLINE_DEG_BAR` (2) stores its graph IDs
inline; larger groups live on the destination machine and are referenced by an
offset. `neighbor_block` only decodes the existing header flags and degree.
It does not decide where to compute or whether to filter.

## Search

`tests/scala_anns.cc` selects the distance/data type, constructs `ScalaSearch`,
and calls `run_recall_sweep`. Search option parsing resides in
`src/anns/anns_param.cc`. Loading and per-ef initialization are in
`include/anns/search/environment.h`.

The per-query control flow is `ScalaSearch::run_query` in
`include/anns/search/traversal.h`:

1. `search_top_index` finds initial candidates on the replicated top index.
2. `select_core_machines` groups seeds by owner. A partition with a fraction
   strictly above `1/MACHINE_NUM` is core; the largest group supplies the leader,
   which is always core. Non-core seeds belong to the leader. `seed_target` is
   shared with fork decoding; the original insertion order is retained.
3. `dispatch_subqueries` starts the local core work and sends QUERY forks to
   remote cores. A non-core issuer yields until the result returns.
4. `select_candidate_batch` takes up to `CAND_GROUP_SIZE` candidates within the
   current bound. `expand_candidate_batch` routes their work and keeps the
   existing `SYNC_BAR` synchronization cadence.
5. After the original coroutine yield, `merge_completed_work` processes local
   neighbor tasks, shared tasks, inline neighbor tasks, completed READs and
   remote results, in that order. Candidate updates can generate more work.
6. Once there are no eligible candidates and both task counters are balanced,
   `advance_termination` runs the existing black/white token protocol. A single
   core can finish directly; multiple cores may pause and be reawakened.
7. `assemble_result` selects top-k and translates graph IDs to original labels.
   The leader returns an END payload to the issuer when needed.
   `take_query_result` keeps the existing one-time result delivery guard.

`CandidateSet` contains the top-index and local graph candidate acceptance
rules. `QuerySynchronization` applies remote candidate updates separately:
receiving a synchronized top candidate is not equivalent to discovering a new
local candidate and must not acquire the latter's side effects.

## Neighbor routing

`NeighborExpansion` owns the routing policy above `ScalaGraphIndex`:

| Neighbors | Action |
| --- | --- |
| Local | Queue local distance computation |
| Remote core, inline IDs | Synchronize IDs to that core |
| Remote core, out-of-line list | Synchronize offset and degree to that core |
| Remote non-core, inline IDs | READ vectors and compute at the current core |
| Remote non-core, out-of-line list | Send a compute task and receive results |

A candidate node that itself lives remotely becomes a `NodeTask`; its owner
expands its adjacency and forwards tasks/results through the original paths.

`PqGate` preserves the existing optional gate on inline remote neighbors during
local-node expansion. It applies only when top-ef is full. The comparison,
shadow mode, channel counts and PQ distance count remain the same. PQ files are
still produced separately by `pq_encode`, loaded in graph order through the
original-label map, and are not regenerated during search.

## State, protocol and runtime

| Module | Responsibility |
| --- | --- |
| `query_state.h` | Query vector, phase, candidate heaps, visited state, task counters, token state, coroutine and pending results |
| `query_sync.h` | Apply decoded core/non-core updates to query state |
| `protocol/query_codec.h` | Existing FORK, END and retained legacy query serializers |
| `protocol/search_messages.h` | Existing TASK, RESULT, NODE_RESULT and SYNC records and their codecs |
| `vec_buffer.h` | READ caches and transport send/receive buffers |
| `task_manager.h` | Thread-local queues, batch packing/unpacking and event delivery |
| `search_protocol.h` | Search message posting, batching and receive polling above the transport |
| `search/runtime.h`, `search/event_handlers.h` | Query admission, event dispatch, coroutine resumption, work stealing and serving peers until the batch finishes |
| `rdma/rdma_comm.*`, `rdma/rdma_context.*` | Shared-memory handshake, one-sided transport, buffer lifecycle and operation accounting |
| `evaluation/recall_runner.h`, `evaluation/search_metrics.h` | Query division, ef sweep, ground-truth accounting and original text/CSV output |

The `search/*.h` files define template methods after the declaration in
`scala_search.h`; consumers include `scala_search.h`. The old `exec_query.h`
and `query_msg.h` remain include facades, not alternate implementations.

Receive order remains TASK → RESULT → NODE_RESULT → COMPUTE → QUERY → CORE_INFO
→ NODE_SYNC → SYNC → READ completions. Synchronization messages still wait until
the fork has initialized the query. Runtime dispatch order remains query
messages → node tasks → node results → compute tasks → read completions → result
messages → subqueries. No coroutine suspension points or batching thresholds
were added or removed.

The dormant `ScalaScheduler` remains dormant. `return_query_result` names the
active END-return operation formerly called `migrate_query`; the wire control
and the `MIGRATE` statistics label remain unchanged.

## Compatibility and evaluation

Index filenames, header widths, packed adjacency fields, vector/label ordering,
wire field ordering, counter locations and CLI defaults are unchanged.
`ef = max(ef, k)`, per-machine integral query division, result ownership and
per-ef reset points remain in place. The shared-memory transport implementation
and vendored dependencies have not been rewritten.

Builds and PQ training are non-deterministic. Future behavior comparisons need
the same existing index and PQ codes, and should compare communication at equal
recall. This refactor does not provide new recall or communication measurements.
