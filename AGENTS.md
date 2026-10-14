# AGENTS.md

pstr holds the plan. Call `context` when starting or resuming, passing the node you pick up as `node`; put your plan in `plan` before durable changes and `report` each step as you finish it.

CoTra is the research prototype behind the paper "CoTra: Towards Efficient and
Scalable Distributed Vector Search with RDMA". It builds a graph ANN index
partitioned across machines and searches it over RDMA. The bar is measurements
you can trust and that can be reproduced from the committed code and scripts,
not production hardening. On the paper cluster that is recall against ground
truth, QPS, and latency. On this machine the NIC is Soft-RoCE on `lo`.
Latency and QPS from that path are not a result. A search run here reports
recall and the RDMA READ/WRITE byte and operation counts.

## Before you change anything

- **A local build runs in the Soft-RoCE container.** The host needs Docker and
  the `rdma_rxe` module. `scripts/softroce/Dockerfile` is ubuntu:22.04 with
  GCC 11, rdma-core, memcached, OpenBLAS, liblapacke, and libaio.
  `docker_test_read.sh` and `docker_e2e.sh` start it `--privileged --network
  host`, mount `/lib/modules`, and set memlock unlimited, so `modprobe
  rdma_rxe` uses the host kernel. One `rxe` attaches to `lo`. N processes
  share it and pass `-m` and `-d rxe0`. If that container did not run, say so.
  Cluster scripts still read datasets under `/data/share/...` and expect a
  real NIC. Those runs are unchanged.
- **Index binaries link LP64 OpenBLAS.** Configure with
  `-DCOTRA_WITH_INDEX=ON`. `third_party/compat/mkl.h` supplies the MKL names
  vendored DiskANN includes. `MKL_INT` is 32-bit, so a GEMM or LAPACK
  dimension must fit in `int`. A partition from this build differs from one
  made with the cluster ILP64 MKL, and the numerics do not have to match the
  paper. faiss is absent from the link line. RDMA-only tests configure
  without the index option. `third_party/diskann/CMakeLists.txt` is upstream
  DiskANN; the file this tree configures is `third_party/CMakeLists.txt`.
- **Most logic is header-only templates** (`include/anns/scala_search.h` is
  ~3.3k lines; also `include/anns/exec_query.h`, `include/index/scala_build.h`).
  Errors only surface when `tests/scala_anns.cc` / `tests/scala_index.cc`
  instantiate them. The build uses `-w -Ofast`: no warnings, fast-math.
- **Cluster size is a compile-time constant.** `MACHINE_NUM` defaults to 16
  in `include/rdma/rdma_config.h` and in the CMake cache. `-DMACHINE_NUM=N`
  overrides both. It sizes many arrays and is the partition count
  (`TARGET_PARTS`, `num_parts`). The `.conf` must list exactly `MACHINE_NUM`
  machines or `AnnsParameter` aborts. An index built with one value cannot be
  read by a binary built with another. The Soft-RoCE runner uses a build
  directory per N (`/tmp/cotra-build-${N}`).
- Limits that are not checked at runtime: search `-t` must be at most
  `MAX_THREAD_NUM` (16, sizes `ExchangeMeta::thd_msg`); `-R` must be at most
  `MAX_GRAPH_DEG` (96, sizes the per-task neighbor `Chunk` used by `scala_v3`).
- **One Soft-RoCE memory region holds at most 1GiB of page-aligned memory.**
  rxe `kzalloc`s a 16-byte entry per 4KiB page into a 4MiB slab, which is
  262144 pages. The cap is per region. `max_mr_size` is unlimited, and raising
  memlock does not move it. `malloc(1GiB)` is offset by the allocator and
  spans one extra page, so it fails; `malloc(1GiB - 4KiB)` succeeds. Search
  registers the real partition. The build-only read buffer in
  `ScalaBuild::init_rdma` is 64MiB and is a different registration. Two
  processes each registering about 1GiB returned `ENOMEM` before search
  started. A larger partition needs several regions, or more processes so
  each region stays under 1GiB.
- Debug and profiling modes are `#define`s at the top of
  `include/anns/search_interface.h`. `COMM_PROFILE` stays defined. It only
  counts RDMA READ and WRITE posts; it does not change the query set or `ef`.
  `report_comm_info` prints one block per machine per ef, and the counters
  are cleared at the start of that ef. The `MB/s` line divides `bytes >> 20`
 by time, so a total under 1 MiB prints as 0.00. Record `ReadBytes`,
 `ReadCnt`, `WriteBytes`, and `WriteCnt`, summed across machines. WRITE
 includes task, result, and release messages. The block also prints
 `WriteType:` bytes and posts per `ControlType`, and one `Chan:` line per
 path that brings an exact distance from another machine (`READ`, `MICRO`
 for inline ids sent to a core machine, `RESULT`, `NODE_RESULT`): entries
 delivered, already visited on arrival, accepted as candidates, and dropped
 by the PQ gate. `scripts/softroce/comm_table.py LOG_DIR N --types --chan
 --at R,...` sums the blocks per ef and interpolates the four totals at a
 recall. `DEBUG`, `PROFILER`, `LAT`,
  and `PROF_*` stay off: several change the workload (`DEBUG` runs 5-20
  queries, `PROF_Q_DISTRI` runs 3, the `PROF_*_DISTRI` macros fix `ef` to
  300). Do not commit those enabled, and do not compare numbers produced
  under different macro sets.
- `third_party/diskann` and `third_party/hnswlib` are vendored as plain files
  (no gitlinks, despite `.gitmodules`). CoTra's own partition/merge helpers are
  also in `namespace diskann`, but live in `include/index/disk_utils.h` and
  `src/index/disk_utils.cc`. Edit those, not the vendored copy.
- Style: `.clang-format` (Google-based, 2-space indent, 80 columns).

## Checking a change

There is no automated test suite; `tests/{rdma,anns,index}/` are ad-hoc
micro-benchmarks and tools. On the paper cluster the correctness signal is
the recall column printed by `test_vs_recall`. When you change build or
search code, compare recall against a run of the previous commit with the
same cluster, `MACHINE_NUM`, macros, index, and flags. On that cluster a QPS
gain with a recall drop at the same `ef` is a regression, not a speedup.
On Soft-RoCE, every search run records recall and, for each ef, the summed
RDMA read/write bytes and operation counts. Latency and QPS are printed and
are not the result: this host is a software RoCE path on `lo`. OpenBLAS
partitions are a different index from one built with the cluster MKL, so
those two tables are not a pair.

The check that runs here is `scripts/softroce/`. `docker_test_read.sh` is
`MACHINE_NUM=2`, `-t 1`, one rxe on `lo`, and a 512MB registration. The
leader's synchronous RDMA READ of machine 1 matches a local fill from 64B
through 8192B, including 268B. The async READ byte compare in `test_read` is
still commented out. An 8192B async post needs 64 work requests and exceeds
`MAX_READ_NUM` (32), so that size is skipped.

`docker_e2e.sh` runs `e2e.sh`. `MACHINE_NUM` defaults to 2. Set
`MACHINE_NUM=4` when the change can affect a neighbor that lives on another
machine. The script writes the synthetic set, builds `scalagraph_v3`,
searches with `scala_v3`, then runs `single` on the merged graph. Pass is
every process exiting 0, machine 0's aggregate recall at ef 100 at least 0.9
and within 0.03 of `single`, and, for N greater than 2, a positive sum of
`all_in_remote_ngh_num`. `check_remote` also requires every id in exactly one
part. A 2-process run can pass with that sum at 0: two replicas and a cut
that shares no boundary neighbors never put a remote neighbor group on the
wire, so its recall does not say the cross-machine path worked. On the
current synthetic set, `MACHINE_NUM=4` had 11988 out-of-line remote-neighbor
ids, 0.715 of ground-truth ids off the issuing machine, and ef 100 recall
0.9985 against `single` 0.9995. The same data at `MACHINE_NUM=2` was 1.0
against 1.0. At 4P, `scala_v3` sits below `single` through the middle of the
ef sweep and the two meet from ef 150. At 2P, `scala_v3` was at least
`single` at every ef. Read a mid-ef gap on 4P against that shape.

`e2e.sh` also runs experiments on an index that already exists:
`SKIP_INDEX=1` searches the index in `WORKDIR`, `SKIP_SINGLE=1` skips
`single` and the pass rule, `SEARCH_ARGS` is appended to the `scala_v3`
command, and `BUILD_DIR` keeps a build across containers. `PQ_CHUNKS` and
`PQ_TAU` turn on the PQ gate below. `docker_e2e.sh` passes these through.

The PQ gate (`--pq_prefix`, `--pq_tau`, `include/anns/pq_codes.h`) keeps
PQ codes of every base vector on every machine. `tests/index/pq_encode`
writes them from `base.bin` with vendored DiskANN; the training sample is
random, so a re-encode gives different codes. When top-ef is full, an
inline neighbor on another machine whose PQ distance exceeds
`lowerBound * (1 + tau)` is not READ or sent to its core machine. A
negative tau counts those neighbors and sends them anyway. Out-of-line
groups and the node-expansion path are not gated: the first carry only an
offset, the second has no lower bound. Every machine holds 1 byte per chunk
per base vector, so the codes grow with the dataset, not the partition.
Compare a gated run with the baseline at the same recall, not the same ef.

`ef` below `k` is raised to `k` (`ef = max(ef, k)` in `scala_search.h`). With
`k` 10, the printed rows for ef 3 through 9 all searched at ef 10.
`scala_v3` moves between runs on those rows. `single` does not. Compare them
as one operating point.

`--sample_percent` defaults to 0.01 in `IndexParameter` and in `e2e.sh`, as
in the cluster scripts. The top HNSW is replicated on every machine. At 1.0
it holds every point, the pre-stage can return ground truth from the issuer
alone, and recall no longer measures cross-machine traversal.

In `coro_sub_query_v5` a machine is core when its share of pre-stage seeds is
strictly above `1/MACHINE_NUM`, and the machine with the most seeds (the
leader) is always core. An exact even split used to leave the core set empty:
nothing was forked, and the issuer waited for an END that never arrived. A
2-process run on this synthetic set hung at the first ef before that rule,
including at `--sample_percent 1.0`. Leave the leader in the core set unless
you replace that liveness guarantee.

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
- `scripts/softroce/`: the container, `gen_synth.cc`, `check_remote.cc`,
 `e2e.sh`, `docker_e2e.sh`, `docker_test_read.sh`, `comm_table.py`, and
 `sift1m_4p.txt` (recorded SIFT1M 4P tables).

## How a run works

Scripts run on every machine at once, from `build/` (they use `../scripts/...`
and `./tests/...`). `clear_memcache` restarts memcached only on the leader
(lines 1-2 of the `.conf`) and resets the `serverNum` / `clientNum` keys used
for rendezvous; run it before every job. Each process finds its machine ID by
matching its first non-loopback IPv4 address against the `.conf`
(`src/rdma/ip_config.cc`), so hosts with several interfaces can pick the wrong
one. `-m` overrides that lookup (`resolve_machine_id`). The Soft-RoCE runner
always passes `-m`, because every process has the same address. The verbs
device defaults to `mlx4_0`; pass `-d <device>` otherwise (`rxe0` on the local
runner). Ethernet, including Soft-RoCE, has LID 0. `modify_qp_to_rtr` fills
the GRH when the link type is Ethernet. `HAVE_MLX5DV` stays off.
`-t` sets the runtime and RDMA thread count; `-T` sets DiskANN build threads.

`-s` defaults to 0. The bin header is the point count, and that count need
not be a whole number of millions. A non-zero `-s` is the count in millions
and must match the header, or both `IndexParameter` and `AnnsParameter`
abort. `-Q` is how many queries are loaded. The default is 10000, and it is
not taken from the query-file header. Under `scala_v3` each machine issues
`qsize / MACHINE_NUM` queries. The division is integral, so a remainder is
dropped. The synthetic runner refuses a query count that does not divide N
and passes `-Q` equal to the file.

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
`ef  recall  latency  QPS` per row. With `COMM_PROFILE` it also prints, per
machine and per ef, `ReadBytes`, `ReadCnt`, `WriteBytes`, and `WriteCnt`.
On Soft-RoCE those four, summed across machines, are the traffic record;
the latency and QPS columns are not. Some scripts have result tables pasted
after `done`; they are informal records, and bash prints "command not found"
for them.

The local runner does not use `clear_memcache`. `e2e.sh` and `inside.sh`
start memcached on `127.0.0.1:21211` and wait until a `set` returns `STORED`.
They kill an existing `memcached` first: a leftover that answers the probe
and then exits will take the port and the next job will fail at rendezvous.
`gen_synth.cc` writes 20000 base vectors and 200 held-out queries, an 8-d
uniform latent mapped linearly into 32-d uint8, so k-means parts are adjacent
and 10-NN lists cross them. Ground truth is brute force, id only, k=10, with
ties broken by the smaller id. `check_remote.cc` reads the N
`<prefix><m>_final_scala.data` files. Record 0 in each file is the entry-point
copy.

## Variants

| Paper name | Build `--graph_type` | Search `--app_type` / `--graph_type` |
|---|---|---|
| CoTra | `scalagraph_v3` | `scala_v3` / `scalagraph_v3` |
| Single machine | `scalagraph_v3` (machine 0 output) | `single` / `vamana` |
| Shard index (random) | `shared_nothing` | `b2` / `shared_nothing` |
| Shard index (k-means) | see known issues | `b2kmeansbatch` / `Kshared_nothing` |
| Global index | unclear | likely `b1`; no committed script |

The local runner builds `scalagraph_v3`, searches `scala_v3`, then runs
`single` / `vamana` on the merged graph. It does not run the shard or global
baselines. The single-machine baseline loads `<prefix>_temp_b1_final.index`,
which is the merged CoTra graph from a `scalagraph_v3` build, not an
independently built Vamana graph. `--scala_v3` and `--scalagraph_v2` are
parsed but unused. `-B` and `-M` (DiskANN DRAM budgets) are required by the
parser.

## Known issues (verify before trusting related numbers)

- **A 2-process pass does not show a remote-neighbor read.** `e2e.sh`
  requires a positive `all_in_remote_ngh_num` sum only when N is greater than
  2. The two-blob set used before the current generator had that sum at 0,
  with recall 1.0 at ef 100 and 0.997 at ef 150. `gen_synth.cc` now writes
  the 8-d latent set. Its cross-machine evidence is the 4P run above.
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
- **The owner of a TASK keeps results by an inverted test.** The issuer
 sets `ef_fill = top_candidates.size() < ef` (still filling); the owner
 keeps a distance when `!task.ef_fill || dist < task.lower_bound`. After
 the pre-stage top-ef is full, so out-of-line results come back
 unfiltered: on SIFT1M 4P at ef 100, 6848 of 154390 RESULT entries were
 accepted. The issuer re-tests every entry, so recall does not depend on
 it; RESULT bytes do.
- `README.md` documents `--dist_fn ip`; the code accepts only `l2` and `mips`
  (plus `cosine` at build time).
- `--app_type b2kmeans` parses, but `test_vs_recall` has no case for it, so it
  prints nothing.
- `--graph_type hnsw_random` / `hnsw_kmeans` parse, but `GraphIndex` leaves
  `b1_graph` null.
- `scripts/deep/vamana_index_*.sh` call `./tests/index/loader`, which no CMake
  target builds.
