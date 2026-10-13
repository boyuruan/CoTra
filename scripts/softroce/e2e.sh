#!/bin/bash
# Build a synthetic uint8 set, then scalagraph_v3 index and scala_v3 search.
# Two processes, one rxe, machine id from -m. Same flags a later dataset uses,
# except -Q matches this query file. Expects the repo at /src, --privileged,
# --network host, and the host /lib/modules.
set -euo pipefail

CONF=/src/scripts/softroce/ip_local.conf
PORT=21211
BUILD=/tmp/cotra-build
DATA=/tmp/cotra-synth
PREFIX=${DATA}/idx/merged
INDEX_BIN=${BUILD}/tests/scala_index
ANNS_BIN=${BUILD}/tests/scala_anns

setup_memcached() {
  if [[ -f /tmp/memcached.pid ]]; then
    kill "$(cat /tmp/memcached.pid)" 2>/dev/null || true
    wait "$(cat /tmp/memcached.pid)" 2>/dev/null || true
    rm -f /tmp/memcached.pid
  fi
  pkill -x memcached 2>/dev/null || true
  sleep 0.3
  # Stay a child of this shell so stderr is captured. -d on Ubuntu returns
  # before the daemon is listening, and a failed bind is easy to miss.
  memcached -u memcached -l 127.0.0.1 -p "${PORT}" -c 1024 \
    >/tmp/memcached.log 2>&1 &
  echo $! >/tmp/memcached.pid
  local i
  for i in 1 2 3 4 5 6 7 8 9 10; do
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

dump_logs() {
  local name=$1
  local lstatus=$2
  local fstatus=$3
  echo "===== ${name} leader exit ${lstatus} (head) ====="
  head -n 40 "/tmp/${name}.m0.log" || true
  echo "===== ${name} leader (tail) ====="
  tail -n 40 "/tmp/${name}.m0.log" || true
  echo "===== ${name} follower exit ${fstatus} (head) ====="
  head -n 40 "/tmp/${name}.m1.log" || true
  echo "===== ${name} follower (tail) ====="
  tail -n 40 "/tmp/${name}.m1.log" || true
  if [[ "${lstatus}" -ne 0 || "${fstatus}" -ne 0 ]]; then
    echo "===== memcached.log ====="
    cat /tmp/memcached.log || true
  fi
}

# Leader in the foreground. Follower gets a short grace after the leader exits.
run_two() {
  local name=$1
  local timeout_s=$2
  local bin=$3
  shift 3
  local llog="/tmp/${name}.m0.log"
  local flog="/tmp/${name}.m1.log"
  local follower lstatus fstatus

  setup_memcached
  stdbuf -oL -eL "${bin}" -m 1 "$@" >"${flog}" 2>&1 &
  follower=$!
  set +e
  stdbuf -oL -eL timeout --foreground "${timeout_s}" \
    "${bin}" -m 0 "$@" >"${llog}" 2>&1
  lstatus=$?
  set -e
  if kill -0 "${follower}" 2>/dev/null; then
    set +e
    timeout --foreground 20 tail --pid="${follower}" -f /dev/null
    set -e
  fi
  if kill -0 "${follower}" 2>/dev/null; then
    kill "${follower}" 2>/dev/null || true
    wait "${follower}" 2>/dev/null || true
    fstatus=124
  else
    set +e
    wait "${follower}"
    fstatus=$?
    set -e
  fi
  dump_logs "${name}" "${lstatus}" "${fstatus}"
  [[ "${lstatus}" -eq 0 && "${fstatus}" -eq 0 ]]
}

recall_ok() {
  # Machine 1 prints: ef \t recall \t <us> \t <qps>
  awk -F'\t' '
    $1 ~ /^[0-9]+$/ && $3 ~ /us/ && $2 ~ /^[0-9]+(\.[0-9]+)?$/ && $2+0 > 0 {
      ok = 1
    }
    END { exit ok ? 0 : 1 }
  ' /tmp/search.m1.log
}

modprobe rdma_rxe
g++ --version | head -n 1
mkdir -p "${DATA}/idx"
g++ -O2 -o /tmp/gen_synth /src/scripts/softroce/gen_synth.cc
/tmp/gen_synth "${DATA}"
read -r N DIM NQ K < "${DATA}/meta.txt"
echo "synthetic meta n=${N} dim=${DIM} nq=${NQ} k=${K}"

if ! cmake -S /src -B "${BUILD}" -DCOTRA_WITH_INDEX=ON -DMACHINE_NUM=2 \
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

COMMON=(
  --config_file "${CONF}"
  --graph_type scalagraph_v3
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
  --sample_percent 1.0
  -t 1
  -d rxe0
)

if ! run_two index 180 "${INDEX_BIN}" "${COMMON[@]}"; then
  echo "soft-roce synthetic index build failed"
  exit 1
fi

if ! run_two search 300 "${ANNS_BIN}" \
  "${COMMON[@]}" \
  --app_type scala_v3 \
  --query_path "${DATA}/query.bin" \
  --gt_path "${DATA}/gt.bin" \
  --res_knn "${K}" \
  -Q "${NQ}"; then
  echo "soft-roce synthetic search failed"
  exit 1
fi

echo "===== search recall lines ====="
grep -E '^[0-9]+' /tmp/search.m1.log || true
if ! recall_ok; then
  echo "soft-roce synthetic search printed no positive recall"
  exit 1
fi

echo "soft-roce synthetic e2e passed"
exit 0
