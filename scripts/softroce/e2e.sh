#!/bin/bash
# Standalone CoTra (scalagraph_v3 index + scala_v3 search) over POSIX /dev/shm.
# MACHINE_NUM processes (default 2), machine id from -m.
# With DATA_DIR unset, builds the synthetic uint8 set. With DATA_DIR set, uses
# that directory's base.bin, query.bin, gt.bin, and meta.txt ("npts dim nq k",
# k is res_knn). The index goes to WORKDIR and logs to LOG_DIR, so DATA_DIR can
# be read-only. Expects the repo at /src and --ipc=host.
# SKIP_INDEX=1 searches the index already in WORKDIR. SEARCH_ARGS is appended to
# the scala_v3 command line. BUILD_DIR keeps the build across containers.
# PQ_CHUNKS > 0 encodes base.bin into WORKDIR/pq<chunks> once and turns on the
# scala_v3 PQ gate with PQ_TAU (negative: count only).
set -euo pipefail

N=${MACHINE_NUM:-2}
SAMPLE_PERCENT=${SAMPLE_PERCENT:-0.01}
SEARCH_TIMEOUT=${SEARCH_TIMEOUT:-300}
INDEX_TIMEOUT=${INDEX_TIMEOUT:-300}
SKIP_INDEX=${SKIP_INDEX:-0}
PQ_CHUNKS=${PQ_CHUNKS:-0}
PQ_TAU=${PQ_TAU:-0}
IB_DEV=${IB_DEV:-shm}
read -ra EXTRA_SEARCH <<< "${SEARCH_ARGS:-}"
LOG_DIR=${LOG_DIR:-/tmp}
WORKDIR=${WORKDIR:-/tmp/cotra-work-${N}}
BUILD=${BUILD_DIR:-/tmp/cotra-build-${N}}
DATA=${DATA_DIR:-/tmp/cotra-synth}
PREFIX=${WORKDIR}/merged
INDEX_BIN=${BUILD}/tests/scala_index
ANNS_BIN=${BUILD}/tests/scala_anns
# Pass rule at this ef: scala_v3 recall >= MIN_RECALL.
CHECK_EF=100
MIN_RECALL=0.99

# Leader in the foreground. Followers get a short grace after the leader exits.
run_all() {
  local name=$1
  local timeout_s=$2
  local bin=$3
  shift 3
  local m lstatus failed=0
  local -a pids status

  for ((m = 1; m < N; m++)); do
    stdbuf -oL -eL "${bin}" -m "${m}" "$@" >"${LOG_DIR}/${name}.m${m}.log" 2>&1 &
    pids[m]=$!
  done
  set +e
  stdbuf -oL -eL timeout --foreground "${timeout_s}" \
    "${bin}" -m 0 "$@" >"${LOG_DIR}/${name}.m0.log" 2>&1
  lstatus=$?
  set -e
  status[0]=${lstatus}
  for ((m = 1; m < N; m++)); do
    if kill -0 "${pids[m]}" 2>/dev/null; then
      set +e
      timeout --foreground 20 tail --pid="${pids[m]}" -f /dev/null
      set -e
    fi
    if kill -0 "${pids[m]}" 2>/dev/null; then
      kill "${pids[m]}" 2>/dev/null || true
      wait "${pids[m]}" 2>/dev/null || true
      status[m]=124
    else
      set +e
      wait "${pids[m]}"
      status[m]=$?
      set -e
    fi
  done
  for ((m = 0; m < N; m++)); do
    echo "===== ${name} m${m} exit ${status[m]} (tail) ====="
    tail -n 25 "${LOG_DIR}/${name}.m${m}.log" || true
    [[ "${status[m]}" -eq 0 ]] || failed=1
  done
  return ${failed}
}

# scala_v3 machine 0 prints the all-machine aggregate: "ef \t recall \t ...".
RECALL_LINE='^[0-9]+ ?\t ?[0-9.]+ ?\t'
recall_rows() {
  awk -v re="${RECALL_LINE}" '$0 ~ re { print $1, $2 }' "$1"
}
recall_at() {
  recall_rows "$1" | awk -v ef="$2" '
    $1 == ef { r = $2 }
    END { if (r == "") exit 1; print r }
  '
}

g++ --version | head -n 1
echo "MACHINE_NUM=${N} SAMPLE_PERCENT=${SAMPLE_PERCENT} IB_DEV=${IB_DEV}"
mkdir -p "${WORKDIR}" "${LOG_DIR}"
g++ -O2 -o /tmp/check_remote /src/scripts/softroce/check_remote.cc
if [[ -z "${DATA_DIR:-}" ]]; then
  mkdir -p "${DATA}"
  g++ -O2 -o /tmp/gen_synth /src/scripts/softroce/gen_synth.cc
  /tmp/gen_synth "${DATA}"
else
  for f in base.bin query.bin gt.bin meta.txt; do
    if [[ ! -f "${DATA}/${f}" ]]; then
      echo "DATA_DIR ${DATA} is missing ${f}"
      exit 1
    fi
  done
fi
read -r NPTS DIM NQ K < "${DATA}/meta.txt"
echo "dataset n=${NPTS} dim=${DIM} nq=${NQ} res_knn=${K}"
if (( NQ % N != 0 )); then
  echo "nq ${NQ} is not divisible by MACHINE_NUM ${N}"
  exit 1
fi

if ! cmake -S /src -B "${BUILD}" -DCOTRA_WITH_INDEX=ON -DMACHINE_NUM="${N}" \
  >"${LOG_DIR}/cmake-cfg.log" 2>&1; then
  echo "cmake configure failed"
  tail -n 80 "${LOG_DIR}/cmake-cfg.log" || true
  exit 1
fi
TARGETS=(scala_index scala_anns)
if (( PQ_CHUNKS > 0 )); then TARGETS+=(pq_encode); fi
if ! cmake --build "${BUILD}" -j "$(nproc)" --target "${TARGETS[@]}" \
  >"${LOG_DIR}/cmake-build.log" 2>&1; then
  echo "cmake build failed"
  tail -n 80 "${LOG_DIR}/cmake-build.log" || true
  exit 1
fi

if (( PQ_CHUNKS > 0 )); then
  PQ_PREFIX=${WORKDIR}/pq${PQ_CHUNKS}
  if [[ ! -f "${PQ_PREFIX}_codes.bin" ]]; then
    if ! "${BUILD}/tests/index/pq_encode" uint8 "${DATA}/base.bin" \
      "${PQ_PREFIX}" "${PQ_CHUNKS}" >"${LOG_DIR}/pq_encode.log" 2>&1; then
      echo "pq_encode failed"
      tail -n 40 "${LOG_DIR}/pq_encode.log" || true
      exit 1
    fi
  fi
  echo "PQ gate: ${PQ_PREFIX}, tau ${PQ_TAU}"
  EXTRA_SEARCH+=(--pq_prefix "${PQ_PREFIX}" --pq_tau "${PQ_TAU}")
fi

# SAMPLE_PERCENT defaults to 0.01 as in the cluster scripts. The top index is
# on every machine; at 1.0 it holds every point and the issuer finds GT locally.
COMMON=(
  --data_type uint8
  --dist_fn l2
  --data_path "${DATA}/base.bin"
  --index_path_prefix "${PREFIX}"
  -R 16
  -L 32
  -B 1
  -M 1
  -T 1
  --scala_v3
  --sample_percent "${SAMPLE_PERCENT}"
  -t 1
  -d "${IB_DEV}"
)
QUERY=(
  --query_path "${DATA}/query.bin"
  --gt_path "${DATA}/gt.bin"
  --res_knn "${K}"
  -Q "${NQ}"
)

if [[ ${SKIP_INDEX} -eq 1 ]]; then
  echo "===== reusing the index in ${WORKDIR} ====="
else
  if ! run_all index "${INDEX_TIMEOUT}" "${INDEX_BIN}" \
    --graph_type scalagraph_v3 "${COMMON[@]}"; then
    echo "standalone shm index build failed"
    exit 1
  fi

  echo "===== out-of-line remote neighbor ids per machine ====="
  REMOTE_SUM=0
  for ((m = 0; m < N; m++)); do
    r=$(awk '/^all_in_remote_ngh_num:/ { print $2 }' \
      "${LOG_DIR}/index.m${m}.log")
    echo "m${m} all_in_remote_ngh_num ${r:-missing}"
    REMOTE_SUM=$((REMOTE_SUM + ${r:-0}))
  done
  echo "sum ${REMOTE_SUM}"
fi

echo "===== partition coverage and GT placement ====="
if ! /tmp/check_remote "${PREFIX}" "${N}" "${DATA}/gt.bin" "${DIM}" \
  | tee "${LOG_DIR}/check_remote.log"; then
  echo "partition check failed"
  exit 1
fi

if ! run_all search "${SEARCH_TIMEOUT}" "${ANNS_BIN}" \
  --graph_type scalagraph_v3 "${COMMON[@]}" --app_type scala_v3 \
  "${QUERY[@]}" "${EXTRA_SEARCH[@]}"; then
  echo "standalone shm search failed"
  exit 1
fi

echo "===== recall: ef scala_v3(all machines) ====="
recall_rows "${LOG_DIR}/search.m0.log"

SCALA_R=$(recall_at "${LOG_DIR}/search.m0.log" "${CHECK_EF}") || SCALA_R=""
if [[ -z "${SCALA_R}" ]]; then
  echo "no recall line at ef ${CHECK_EF}"
  exit 1
fi
echo "ef ${CHECK_EF}: scala_v3 ${SCALA_R}"
if (( SKIP_INDEX == 0 && N > 2 && REMOTE_SUM == 0 )); then
  echo "no out-of-line remote neighbor ids"
  exit 1
fi
if ! awk -v s="${SCALA_R}" -v lo="${MIN_RECALL}" \
    'BEGIN { exit !(s >= lo) }'; then
  echo "scala_v3 recall ${SCALA_R} below ${MIN_RECALL}"
  exit 1
fi

echo "standalone shm e2e passed (MACHINE_NUM=${N})"
exit 0
