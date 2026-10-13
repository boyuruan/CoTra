#!/bin/bash
# Build a synthetic uint8 set, then scalagraph_v3 index and scala_v3 search,
# then single on the merged graph as the reference. MACHINE_NUM processes
# (default 2), one rxe, machine id from -m. Same flags a later dataset uses,
# except -Q matches this query file. Expects the repo at /src, --privileged,
# --network host, and the host /lib/modules.
set -euo pipefail

N=${MACHINE_NUM:-2}
SAMPLE_PERCENT=${SAMPLE_PERCENT:-0.01}
SEARCH_TIMEOUT=${SEARCH_TIMEOUT:-300}
CONF=/tmp/ip_local_${N}.conf
PORT=21211
BUILD=/tmp/cotra-build-${N}
DATA=/tmp/cotra-synth
PREFIX=${DATA}/idx${N}/merged
INDEX_BIN=${BUILD}/tests/scala_index
ANNS_BIN=${BUILD}/tests/scala_anns
# Pass rule at this ef: scala_v3 recall >= MIN_RECALL and >= single - MAX_GAP.
CHECK_EF=100
MIN_RECALL=0.9
MAX_GAP=0.03

setup_memcached() {
  if [[ -f /tmp/memcached.pid ]]; then
    kill "$(cat /tmp/memcached.pid)" 2>/dev/null || true
    wait "$(cat /tmp/memcached.pid)" 2>/dev/null || true
    rm -f /tmp/memcached.pid
  fi
  pkill -x memcached 2>/dev/null || true
  # A memcached from an earlier run is not our child, so wait cannot reap it.
  # Until it exits it holds the port and answers the readiness probe.
  local i pid
  for i in $(seq 50); do
    pgrep -x memcached >/dev/null || break
    sleep 0.1
  done
  # Stay a child of this shell so stderr is captured. -d on Ubuntu returns
  # before the daemon is listening, and a failed bind is easy to miss.
  memcached -u memcached -l 127.0.0.1 -p "${PORT}" -c 1024 \
    >/tmp/memcached.log 2>&1 &
  pid=$!
  echo "${pid}" >/tmp/memcached.pid
  for i in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "${pid}" 2>/dev/null || break
    if printf 'set serverNum 0 0 1\r\n0\r\nquit\r\n' \
        | nc -w 2 127.0.0.1 "${PORT}" | grep -q STORED; then
      printf 'set clientNum 0 0 1\r\n0\r\nquit\r\n' \
        | nc -w 2 127.0.0.1 "${PORT}" >/dev/null
      return 0
    fi
    sleep 0.2
  done
  echo "memcached did not accept a set"
  cat /tmp/memcached.log || true
  return 1
}

link_rxe() {
  rdma link del rxe0 2>/dev/null || true
  rdma link add rxe0 type rxe netdev lo
  rdma link show
}

# Machine names are placeholders; -m picks the id.
write_conf() {
  local m
  {
    echo 127.0.0.1
    echo "${PORT}"
    for ((m = 0; m < N; m++)); do echo "local${m}=${m}"; done
  } >"${CONF}"
}

# Leader in the foreground. Followers get a short grace after the leader exits.
run_all() {
  local name=$1
  local timeout_s=$2
  local bin=$3
  shift 3
  local m lstatus failed=0
  local -a pids status

  setup_memcached
  for ((m = 1; m < N; m++)); do
    stdbuf -oL -eL "${bin}" -m "${m}" "$@" >"/tmp/${name}.m${m}.log" 2>&1 &
    pids[m]=$!
  done
  set +e
  stdbuf -oL -eL timeout --foreground "${timeout_s}" \
    "${bin}" -m 0 "$@" >"/tmp/${name}.m0.log" 2>&1
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
    tail -n 25 "/tmp/${name}.m${m}.log" || true
    [[ "${status[m]}" -eq 0 ]] || failed=1
  done
  if [[ ${failed} -ne 0 ]]; then
    echo "===== memcached.log ====="
    cat /tmp/memcached.log || true
  fi
  return ${failed}
}

# scala_v3 machine 0 prints the all-machine aggregate: "ef \t recall \t ...".
# single prints "ef\trecall\t<us> us\t...". Partition-size lines have no tab.
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

modprobe rdma_rxe
g++ --version | head -n 1
echo "MACHINE_NUM=${N} SAMPLE_PERCENT=${SAMPLE_PERCENT}"
write_conf
mkdir -p "${DATA}/idx${N}"
g++ -O2 -o /tmp/gen_synth /src/scripts/softroce/gen_synth.cc
g++ -O2 -o /tmp/check_remote /src/scripts/softroce/check_remote.cc
/tmp/gen_synth "${DATA}"
read -r NPTS DIM NQ K < "${DATA}/meta.txt"
echo "synthetic meta n=${NPTS} dim=${DIM} nq=${NQ} k=${K}"
if (( NQ % N != 0 )); then
  echo "nq ${NQ} is not divisible by MACHINE_NUM ${N}"
  exit 1
fi

if ! cmake -S /src -B "${BUILD}" -DCOTRA_WITH_INDEX=ON -DMACHINE_NUM="${N}" \
  >/tmp/cmake-cfg.log 2>&1; then
  echo "cmake configure failed"
  tail -n 80 /tmp/cmake-cfg.log || true
  exit 1
fi
if ! cmake --build "${BUILD}" -j "$(nproc)" --target scala_index scala_anns \
  >/tmp/cmake-build.log 2>&1; then
  echo "cmake build failed"
  tail -n 80 /tmp/cmake-build.log || true
  exit 1
fi

link_rxe

# SAMPLE_PERCENT defaults to 0.01 as in the cluster scripts. The top index is
# on every machine; at 1.0 it holds every point and the issuer finds GT locally.
COMMON=(
  --config_file "${CONF}"
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
  -d rxe0
)
QUERY=(
  --query_path "${DATA}/query.bin"
  --gt_path "${DATA}/gt.bin"
  --res_knn "${K}"
  -Q "${NQ}"
)

if ! run_all index 300 "${INDEX_BIN}" --graph_type scalagraph_v3 \
  "${COMMON[@]}"; then
  echo "soft-roce synthetic index build failed"
  exit 1
fi

echo "===== out-of-line remote neighbor ids per machine ====="
REMOTE_SUM=0
for ((m = 0; m < N; m++)); do
  r=$(awk '/^all_in_remote_ngh_num:/ { print $2 }' "/tmp/index.m${m}.log")
  echo "m${m} all_in_remote_ngh_num ${r:-missing}"
  REMOTE_SUM=$((REMOTE_SUM + ${r:-0}))
done
echo "sum ${REMOTE_SUM}"

echo "===== partition coverage and GT placement ====="
if ! /tmp/check_remote "${PREFIX}" "${N}" "${DATA}/gt.bin" "${DIM}" \
  | tee /tmp/check_remote.log; then
  echo "partition check failed"
  exit 1
fi

if ! run_all search "${SEARCH_TIMEOUT}" "${ANNS_BIN}" \
  --graph_type scalagraph_v3 "${COMMON[@]}" --app_type scala_v3 \
  "${QUERY[@]}"; then
  echo "soft-roce synthetic search failed"
  exit 1
fi

set +e
stdbuf -oL -eL timeout --foreground 300 "${ANNS_BIN}" -m 0 \
  --graph_type vamana "${COMMON[@]}" --app_type single "${QUERY[@]}" \
  >/tmp/single.log 2>&1
SINGLE_STATUS=$?
set -e
if [[ ${SINGLE_STATUS} -ne 0 ]]; then
  echo "single on the merged graph exited ${SINGLE_STATUS}"
  tail -n 40 /tmp/single.log || true
  exit 1
fi

echo "===== recall: ef scala_v3(all machines) single(merged graph) ====="
join <(recall_rows /tmp/search.m0.log | sort -k1,1) \
     <(recall_rows /tmp/single.log | sort -k1,1) | sort -n

SCALA_R=$(recall_at /tmp/search.m0.log "${CHECK_EF}") || SCALA_R=""
SINGLE_R=$(recall_at /tmp/single.log "${CHECK_EF}") || SINGLE_R=""
if [[ -z "${SCALA_R}" || -z "${SINGLE_R}" ]]; then
  echo "no recall line at ef ${CHECK_EF}"
  exit 1
fi
echo "ef ${CHECK_EF}: scala_v3 ${SCALA_R} single ${SINGLE_R}"
if (( N > 2 && REMOTE_SUM == 0 )); then
  echo "no out-of-line remote neighbor ids"
  exit 1
fi
if ! awk -v s="${SCALA_R}" -v b="${SINGLE_R}" -v lo="${MIN_RECALL}" \
    -v gap="${MAX_GAP}" 'BEGIN { exit !(s >= lo && s >= b - gap) }'; then
  echo "scala_v3 recall below ${MIN_RECALL}" \
    "or more than ${MAX_GAP} under single"
  exit 1
fi

echo "soft-roce synthetic e2e passed (MACHINE_NUM=${N})"
exit 0
