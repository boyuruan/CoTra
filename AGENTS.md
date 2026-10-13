# AGENTS.md

pstr holds the plan. Call `context` when starting or resuming, passing the node you pick up as `node`; put your plan in `plan` before durable changes and `report` each step as you finish it.

CoTra is the research prototype behind the paper "CoTra: Towards Efficient and
Scalable Distributed Vector Search with RDMA". It builds a graph ANN index
partitioned across machines and searches it over RDMA. The bar is measurements
you can trust (recall against ground truth, QPS, latency) and that can be
reproduced from the committed code and scripts, not production hardening.

## Before you change anything

- **You almost certainly cannot build or run it here.** `CMakeLists.txt` and
  `third_party/CMakeLists.txt` hard-code the original cluster: the g++ 11
  binary, Intel MKL and `libiomp5` under `/data/share/users/xyzhi/...`, the
  rdma-core headers under `/data/share/project/grasper/...`, `libibverbs` /
  `librdmacm` under `/usr/lib/x86_64-linux-gnu`, and a static `libfaiss.a`.
  Running also needs RDMA NICs, memcached, and datasets under `/data/share/...`.
  If you could not compile, say so instead of implying the change was checked.
- **Most logic is header-only templates** (`include/anns/scala_search.h` is
  ~3.3k lines; also `include/anns/exec_query.h`, `include/index/scala_build.h`).
  Errors only surface when `tests/scala_anns.cc` / `tests/scala_index.cc`
  instantiate them. The build uses `-w -Ofast`: no warnings, fast-math.
- **Cluster size is a compile-time constant.** `MACHINE_NUM` in
  `include/rdma/rdma_config.h` (currently 16) sizes many arrays and is also the
  partition count (`TARGET_PARTS`, `num_parts`). The `.conf` must list exactly
  `MACHINE_NUM` machines or `AnnsParameter` aborts. An 8P experiment needs a
  rebuild with `MACHINE_NUM 8`, and its index cannot be used by a 16P binary.
- Limits that are not checked at runtime: search `-t` must be at most
  `MAX_THREAD_NUM` (16, sizes `ExchangeMeta::thd_msg`); `-R` must be at most
  `MAX_GRAPH_DEG` (96, sizes the per-task neighbor `Chunk` used by `scala_v3`).
- Debug and profiling modes are `#define`s at the top of
  `include/anns/search_interface.h` (`DEBUG`, `PROFILER`, `LAT`, `PROF_*`).
  Several change the workload (`DEBUG` runs 5-20 queries, `PROF_Q_DISTRI` runs
  3, the `PROF_*_DISTRI` macros fix `ef` to 300). Do not commit them enabled,
  and do not compare numbers produced under different macro sets.
- `third_party/diskann` and `third_party/hnswlib` are vendored as plain files
  (no gitlinks, despite `.gitmodules`). CoTra's own partition/merge helpers are
  also in `namespace diskann`, but live in `include/index/disk_utils.h` and
  `src/index/disk_utils.cc`. Edit those, not the vendored copy.
- Style: `.clang-format` (Google-based, 2-space indent, 80 columns).

## Checking a change

There is no automated test suite; `tests/{rdma,anns,index}/` are ad-hoc
micro-benchmarks and tools. The correctness signal is the recall column printed
by `test_vs_recall`. When you change build or search code, compare the
recall/QPS table against a run of the previous commit with the same cluster,
`MACHINE_NUM`, macros, index, and flags. A QPS gain with a recall drop at the
same `ef` is a regression, not a speedup.

## Layout

- `tests/scala_index.cc`: distributed index build entry (`ScalaBuild`).
- `tests/scala_anns.cc`: search/benchmark entry (`ScalaSearch`, then
  `test_vs_recall`).
- `include/index/`: parameters and output paths (`index_param.h`,
  `src/index/index_param.cc`), build (`scala_build.h`, `src/index/scala_build.cc`),
  CoTra graph format (`scala_graph.h`), baseline formats (`b1_graph.h`,
  `vamana/vamana.h`), HNSW top index (`top_index.h`), k-means (`partition.h`).
- `include/anns/`: search (`scala_search.h`), benchmark driver
  (`exec_query.h`), query/task state (`query_msg.h`, `task_manager.h`),
  distances (`space_l2.h`, `space_ip.h`), tuning constants (`anns_config.h`).
- `include/rdma/`, `src/rdma/`: verbs wrapper (`rdma_comm`), memcached-based
  rendezvous (`server_connect`), `.conf` parsing (`ip_config`).
- `coromem/`: coroutine runtime and thread pool (`do_all`, `on_each`,
  `PerThreadStorage`).
- `scripts/`: cluster configs (`*.conf`) and experiment scripts named
  `scripts/<dataset>/<N>M_<P>P_<variant>_{index,anns}.sh`.

## How a run works

Scripts run on every machine at once, from `build/` (they use `../scripts/...`
and `./tests/...`). `clear_memcache` restarts memcached only on the leader
(lines 1-2 of the `.conf`) and resets the `serverNum` / `clientNum` keys used
for rendezvous; run it before every job. Each process finds its machine ID by
matching its first non-loopback IPv4 address against the `.conf`
(`src/rdma/ip_config.cc`), so hosts with several interfaces can pick the wrong
one. The verbs device defaults to `mlx4_0`; pass `-d <device>` otherwise.
`-t` sets the runtime and RDMA thread count; `-T` sets DiskANN build threads.

Build (`scala_index`): the leader partitions the data (k-means with 2 replicas
for `scalagraph_v3`, random for `shared_nothing`), each machine builds a Vamana
graph for its partition with DiskANN, then post-processing depends on
`graph_type`. CoTra merges replicas into `<prefix><m>_final_scala.{index,data}`
on each machine; machine 0 also writes `<prefix>_temp_b1_final.index` and
`<prefix>_top.index`. Shared-nothing writes
`<prefix>_mem.index_tempFiles_subshard-<m>_b2.index` plus the top index. All
paths derive from `--index_path_prefix` in `IndexParameter`'s constructor.

Search (`scala_anns`): loads the index, connects RDMA, loads ground truth, then
sweeps a hard-coded `ef` list in `test_vs_recall`, printing
`ef  recall  latency  QPS` per row. `-s` (millions) must equal the base file's
vector count. Under `scala_v3` each machine issues `qsize / MACHINE_NUM`
queries. Some scripts have result tables pasted after `done`; they are informal
records, and bash prints "command not found" for them.

## Variants

| Paper name | Build `--graph_type` | Search `--app_type` / `--graph_type` |
|---|---|---|
| CoTra | `scalagraph_v3` | `scala_v3` / `scalagraph_v3` |
| Single machine | `scalagraph_v3` (machine 0 output) | `single` / `vamana` |
| Shard index (random) | `shared_nothing` | `b2` / `shared_nothing` |
| Shard index (k-means) | see known issues | `b2kmeansbatch` / `Kshared_nothing` |
| Global index | unclear | likely `b1`; no committed script |

The single-machine baseline loads `<prefix>_temp_b1_final.index`, which is the
merged CoTra graph from a `scalagraph_v3` build, not an independently built
Vamana graph. `--scala_v3` and `--scalagraph_v2` are parsed but unused. `-B` and
`-M` (DiskANN DRAM budgets) are required by the parser.

## Known issues (verify before trusting related numbers)

- **The k-means shard baseline does not run end to end from committed code.**
  Building with `Kshared_nothing` does k-means partitioning, but
  `tests/scala_index.cc` has no post-build branch for it, so no b2 graph,
  partition maps, or top index are written. Building with `shared_nothing`
  writes them but partitions randomly. At search time `GraphIndex` creates
  `top_index` for `Kshared_nothing` but not for `shared_nothing`, and
  `b2kmeansbatch` dereferences it
  in `dispatch_kmeans_query_start`. Most `*_Kshnot_anns.sh` scripts pass
  `shared_nothing` (only `deep/100M_16P_*` pass `Kshared_nothing`), and
  `sift/10M_16P_Kshnot_anns.sh` and `deep/10M_16P_Kshnot_anns.sh` run plain `b2`.
- `README.md` documents `--dist_fn ip`; the code accepts only `l2` and `mips`
  (plus `cosine` at build time).
- `--app_type b2kmeans` parses, but `test_vs_recall` has no case for it, so it
  prints nothing.
- `--graph_type hnsw_random` / `hnsw_kmeans` parse, but `GraphIndex` leaves
  `b1_graph` null.
- `scripts/deep/vamana_index_*.sh` call `./tests/index/loader`, which no CMake
  target builds.
