# CoTra + shared-memory transport

This branch is the standalone CoTra research prototype: a partitioned graph
(`scalagraph_v3`), cooperative search (`scala_v3`), and a POSIX `/dev/shm`
transport that simulates one-sided RDMA READ and WRITE_WITH_IMM. It does not
require InfiniBand hardware, verbs libraries, or memcached.

The experimental outputs are recall against ground truth and communication
bytes/operation counts summed across all processes. Shared-memory latency and
QPS are not RDMA performance results.

## Read the algorithm

- [Build pipeline](src/index/scala_build.cc): partition vectors, build local
  graphs, create ID maps, merge replicas, encode the distributed graph, and
  build the replicated top index.
- [Query pipeline](include/anns/search/traversal.h): search the top index,
  select core machines, dispatch subqueries, expand candidates, consume work,
  detect global termination, and return top-k labels.
- [Algorithm and module guide](docs/algorithm.md): stage responsibilities,
  data flow, task routing, state ownership, and preserved contracts.
- [Refactor notes](docs/refactor-notes.md): suspected pre-existing defects
  intentionally left unchanged and the limits of validation.
- [Repository working instructions](AGENTS.md): evaluation invariants and
  shared-memory transport details.

## Build only

The supported local environment is the `cotra-softroce` container (Ubuntu 22.04,
GCC 11, LP64 OpenBLAS, LAPACKE, Boost, and libaio). For example, build the binaries
without running any tests or workloads:

```bash
docker build -t cotra-softroce -f scripts/softroce/Dockerfile scripts/softroce
docker run --rm -v "$PWD":/src:ro cotra-softroce bash -lc \
  'cmake -S /src -B /tmp/cotra-build-4 -DCOTRA_WITH_INDEX=ON -DMACHINE_NUM=4 &&
   cmake --build /tmp/cotra-build-4 --target scala_index scala_anns pq_encode -j 4'
```

`MACHINE_NUM` is the compile-time partition count. The same value must be used
when building and reading an index. Executables retain their existing names
and locations under `tests/`; these include the build and evaluation entry
points, not just test programs.

## Execution conventions

Each process receives `-m <0..MACHINE_NUM-1>`. Use `-d shm[:tag]` to select the
shared-memory segment prefix and use a distinct tag for concurrent jobs with
the same process count. All processes in one job must use the same thread count.

The existing scripts in `scripts/softroce/` combine data preparation, index
construction, search and smoke checks. They execute workloads and are separate
from the build-only command above. Run them only when evaluation is intended.
Existing CLI flags, index filenames, index layouts, wire formats, and log
formats are retained by the refactor.

## License

[MIT](LICENSE).
