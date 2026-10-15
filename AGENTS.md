# AGENTS.md

pstr holds the plan. Call `context` when starting or resuming, passing the node you pick up as `node`; put your plan in `plan` before durable changes and `report` each step as you finish it.

This branch (`base-algo`) is the standalone **CoTra + `/dev/shm`** prototype derived from the paper "CoTra: Towards Efficient and Scalable Distributed Vector Search with RDMA". All baseline variants (`single` / `vamana`, `b1`, `b1_async`, `b2` / `shared_nothing`, `b2kmeans` / `Kshared_nothing`), InfiniBand verbs (`libibverbs`, `librdmacm`), and `memcached` rendezvous have been removed. Only the CoTra partitioned graph (`scalagraph_v3` build + `scala_v3` search) and the POSIX shared-memory one-sided transport (`RdmaCommunication::com_init_shm` + `RdmaContext` over `/dev/shm`) remain.

Latency and QPS from shared memory are not a result. A search run reports recall against ground truth and the simulated RDMA READ/WRITE byte and operation counts (`ReadBytes`, `ReadCnt`, `WriteBytes`, `WriteCnt`), summed across all `MACHINE_NUM` processes.

## Before you change anything

- **A local build runs in the container (`cotra-softroce`).** `scripts/softroce/Dockerfile` provides Ubuntu 22.04 with GCC 11, OpenBLAS, liblapacke, Boost, and libaio. `scripts/softroce/docker_e2e.sh` runs it with `--ipc=host` and `--ulimit memlock=-1:-1` so N processes share `/dev/shm` without `rdma_rxe` or `memcached`.
- **Index binaries link LP64 OpenBLAS.** `COTRA_WITH_INDEX` defaults to `ON`. `third_party/compat/mkl.h` supplies the MKL names vendored DiskANN includes. `MKL_INT` is 32-bit, so a GEMM or LAPACK dimension must fit in `int`. `third_party/diskann/CMakeLists.txt` is upstream DiskANN; the file this tree configures is `third_party/CMakeLists.txt`.
- **Most logic is header-only templates** (`include/anns/scala_search.h`, `include/anns/exec_query.h`, `include/index/scala_build.h`, `include/index/scala_graph.h`). Errors only surface when `tests/scala_anns.cc` / `tests/scala_index.cc` instantiate them. The build uses `-w -Ofast`: no warnings, fast-math.
- **Cluster size is a compile-time constant.** `MACHINE_NUM` defaults to 16 in `include/rdma/rdma_config.h` and in the CMake cache. `-DMACHINE_NUM=N` overrides both. It sizes many arrays and is the partition count (`TARGET_PARTS`, `num_parts`). An index built with one value cannot be read by a binary built with another. The runner uses a build directory per N (`/tmp/cotra-build-${N}`).
- **Machine ID is passed directly via `-m <0..MACHINE_NUM-1>`.** No `.conf` file or IP lookup is needed. `-d shm[:tag]` (default `shm`) sets the `/dev/shm` segment prefix; any other `-d` value aborts.
- Limits that are not checked at runtime: `-R` must be at most `MAX_GRAPH_DEG` (96, sizes the per-task neighbor `Chunk` used by `scala_v3`). All `MACHINE_NUM` processes in a job must pass the same `-t`.
- **Debug and profiling modes** are `#define`s at the top of `include/anns/search_interface.h`. `COMM_PROFILE` stays defined. It counts simulated RDMA READ and WRITE posts; it does not change the query set or `ef`. `report_comm_info` prints one block per machine per ef, and the counters are cleared at the start of that ef. Record `ReadBytes`, `ReadCnt`, `WriteBytes`, and `WriteCnt`, summed across machines. The block also prints `WriteType:` bytes and posts per `ControlType`, `Chan:` lines (`READ`, `MICRO`, `RESULT`, `NODE_RESULT`), `Query:` (`FORK`, `MIGRATE`), `Sync:`, and `Work:`. `scripts/softroce/comm_table.py LOG_DIR N --types --chan --sections --at R,...` sums the blocks per ef and interpolates the four totals at a recall. `DEBUG`, `PROFILER`, `LAT`, and `PROF_*` stay off.
- `third_party/diskann` and `third_party/hnswlib` are vendored as plain files. CoTra's own partition/merge helpers are in `include/index/disk_utils.h`, `src/index/disk_utils.cc`, `include/index/partition.h`, and `src/index/partition.cc`. Edit those, not the vendored copy.
- Style: `.clang-format` (Google-based, 2-space indent, 80 columns).

## Algorithm & Evaluation Invariants

- **Index build is non-deterministic; compare A/B on one index (`SKIP_INDEX=1`).** `gen_synth.cc` is seeded, but the k-means/Vamana build is not: three 4P builds of the same synthetic set put 0.715, 0.784, and 0.8125 of ground-truth ids off the issuer. Always evaluate both arms of an A/B change against the same built index in `WORKDIR` using `SKIP_INDEX=1`.
- **Core machine selection and leader liveness (`coro_sub_query_v5`).** A machine is core when its share of pre-stage seeds is strictly above `1/MACHINE_NUM`, and the machine with the most seeds (the leader) is always core. An exact even split without that rule leaves the core set empty, nothing is forked, and the issuer hangs waiting for an END that never arrives. Keep the leader in the core set unless you replace that liveness guarantee.
- **Wire formats and seed derivation (`QueryMsg` and `SyncMsg`).** `QueryMsg::fork`, `QueryMsg::serialize_end` (`include/anns/query_msg.h`), and `SyncMsg::serialize` (`include/anns/vec_buffer.h`) carry only what the receiver reads. A fork receiver derives its seeds from the top list by the issuer's split rule in `coro_sub_query_v5` (entries on the receiver, plus entries on non-core machines when the receiver is the leader), using `TaskManager::machine_of`. If you change that split rule, change it in both places or the cores start from different seeds. Degrees travel as 1 byte and counts as 2 bytes; `SyncMsg::serialize` aborts if either overflows.
- **PQ gate (`--pq_prefix`, `--pq_tau`, `include/anns/pq_codes.h`).** `tests/index/pq_encode` writes PQ codes from `base.bin` with vendored DiskANN; the training sample is random, so a re-encode gives different codes. When top-ef is full, an inline neighbor on another machine whose PQ distance exceeds `lowerBound * (1 + tau)` is not READ or sent to its core machine (negative `tau` counts those neighbors and sends them anyway). Compare a gated run with the baseline at the same recall, not the same `ef`.
- **`ef` floor and query division.** `ef` below `k` is raised to `k` (`ef = max(ef, k)` in `scala_search.h`); with `k=10`, rows for `ef` 3 through 9 all search at `ef=10`. `--sample_percent` defaults to `0.01`; at `1.0` the replicated top HNSW holds every point and recall no longer measures cross-machine traversal. Each machine issues `qsize / MACHINE_NUM` queries (integral division, remainder dropped).
- **Historical `sift1m_4p.txt` note.** `scripts/softroce/sift1m_4p.txt` was recorded on `rxe0` before the `can_apply_sync` hold commit. Because a `/dev/shm` READ or WRITE completes when posted, search interleaves differently: above `ef=150`, `-d shm` is 5–9% lower in `ReadBytes` and 7–10% lower in `WriteCnt` than `rxe0`. Do not compare new `/dev/shm` numbers directly against `sift1m_4p.txt`; run both arms of any comparison on `/dev/shm`.

## Checking a change

`scripts/softroce/docker_e2e.sh` runs `scripts/softroce/e2e.sh`. `MACHINE_NUM` defaults to 2; set `MACHINE_NUM=4` when the change can affect a neighbor that lives on another machine. A 2-process run can have `all_in_remote_ngh_num` at 0 because two replicas cover 2 partitions, so 4P is required to exercise cross-machine out-of-line neighbor traversal.

The script writes the synthetic set (`gen_synth.cc`), builds `scalagraph_v3` (`scala_index`), checks partition coverage and remote neighbor counts (`check_remote.cc`), and searches with `scala_v3` (`scala_anns`). Pass is every process exiting 0, machine 0's aggregate recall at `ef=100` at least `0.99` (`MIN_RECALL`), and, for N greater than 2, a positive sum of `all_in_remote_ngh_num`. `check_remote` also requires every id in exactly one part. Because there is no single-machine baseline in this tree, `MIN_RECALL=0.99` is a smoke check; subtle recall regressions must be checked via A/B on the same index (`SKIP_INDEX=1`).

`e2e.sh` also runs experiments on an index that already exists: `SKIP_INDEX=1` searches the index in `WORKDIR`, `SEARCH_ARGS` is appended to the `scala_v3` command, and `BUILD_DIR` keeps a build across containers. `PQ_CHUNKS` and `PQ_TAU` turn on the PQ gate. `docker_e2e.sh` passes these through.

## Shared-Memory Transport (`/dev/shm`)

`-d shm[:tag]` (or omitting `-d`) selects the POSIX shared-memory transport (`RdmaCommunication::com_init_shm` and `RdmaContext` in `include/rdma/` and `src/rdma/`). N processes simulate one-sided `RDMA_READ` (`memcpy` from the peer's mapped partition region) and `RDMA_WRITE_WITH_IMM` (payload copy into the peer's `ShmThreadRegion::recv_buf` slot plus a lock-free `ShmSpscRing` push of `(imm, len)`).

`com_init` copies the local partition into the shared segment, so its owner must not write the partition afterwards. Segments are named `<prefix>_n<N>_m<m>`, where `<prefix>` is `/dev/shm/<tag>` (or `<tag>` if absolute), defaulting to `/dev/shm/cotra_shm` (`/tmp/cotra_shm` when `/dev/shm` is under 512MiB). During `com_init_shm`, peers handshake via `ShmSegmentHeader` (`instance_id`, `ack_from`, `ack_target`, and `all_connected`), remapping if a leftover file's inode changes, and each process unlinks its segment once all peers have connected. Two concurrent jobs with the same N need different tags (`-d shm:job1`, `-d shm:job2`).

## Layout

- `tests/scala_index.cc`: distributed CoTra index build entry (`ScalaBuild`).
- `tests/scala_anns.cc`: distributed CoTra search/benchmark entry (`ScalaSearch`, then `test_vs_recall`).
- `tests/index/pq_encode.cc`: PQ codebook training and encoding tool for the PQ gate.
- `include/index/`, `src/index/`: parameters and output paths (`index_param`), build (`scala_build`), CoTra graph format (`scala_graph.h`, `graph_index.h`), HNSW top index (`top_index`), k-means partition and merge (`partition`, `disk_utils`).
- `include/anns/`, `src/anns/`: search (`scala_search.h`), benchmark driver (`exec_query.h`), query/task state (`query_msg.h`, `task_manager.h`, `vec_buffer.h`), PQ gate (`pq_codes.h`), distances (`space_l2.h`, `space_ip.h`), tuning constants (`anns_config.h`).
- `include/rdma/`, `src/rdma/`: POSIX `/dev/shm` one-sided READ/WRITE engine (`rdma_comm`, `rdma_context`, `rdma_param`, `rdma_config.h`, `profiler.h`).
- `coromem/`: coroutine runtime and thread pool (`do_all`, `on_each`, `PerThreadStorage`).
- `scripts/softroce/`: container (`Dockerfile`), `gen_synth.cc`, `check_remote.cc`, `e2e.sh`, `docker_e2e.sh`, `comm_table.py`, and `sift1m_4p.txt`.
