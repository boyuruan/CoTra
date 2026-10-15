#!/bin/bash
# Compile and run standalone CoTra (scalagraph_v3 / scala_v3) over /dev/shm in Docker.
# MACHINE_NUM defaults to 2.
# DATA_DIR, when set, is a prepared dataset mounted read-only; DATA_MOUNT is
# an extra host path that files inside DATA_DIR may symlink to. HOST_LOG and
# HOST_WORK receive /logs and /work. SEARCH_TIMEOUT, INDEX_TIMEOUT,
# SKIP_INDEX, SEARCH_ARGS, BUILD_DIR, PQ_CHUNKS, PQ_TAU, and IB_DEV are passed
# through when set (see e2e.sh).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)

docker build -t cotra-softroce \
  -f "${ROOT}/scripts/softroce/Dockerfile" \
  "${ROOT}/scripts/softroce"

run=(docker run --rm --ipc=host --ulimit memlock=-1:-1)
run+=(-e "MACHINE_NUM=${MACHINE_NUM:-2}")
run+=(-v "${ROOT}":/src:ro)

for name in SAMPLE_PERCENT SEARCH_TIMEOUT INDEX_TIMEOUT DATA_DIR \
  SKIP_INDEX SEARCH_ARGS BUILD_DIR PQ_CHUNKS PQ_TAU IB_DEV; do
  if [[ -n "${!name:-}" ]]; then
    run+=(-e "${name}=${!name}")
  fi
done
if [[ -n "${DATA_DIR:-}" ]]; then
  run+=(-v "${DATA_DIR}:${DATA_DIR}:ro")
fi
if [[ -n "${DATA_MOUNT:-}" ]]; then
  run+=(-v "${DATA_MOUNT}:${DATA_MOUNT}:ro")
fi
if [[ -n "${HOST_LOG:-}" ]]; then
  mkdir -p "${HOST_LOG}"
  run+=(-v "${HOST_LOG}:/logs" -e LOG_DIR=/logs)
fi
if [[ -n "${HOST_WORK:-}" ]]; then
  mkdir -p "${HOST_WORK}"
  run+=(-v "${HOST_WORK}:/work" -e WORKDIR=/work)
fi

run+=(cotra-softroce bash /src/scripts/softroce/e2e.sh)
"${run[@]}"
