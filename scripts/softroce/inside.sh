#!/bin/bash
# Build and run a 2-process test_read against one Soft-RoCE device.
# Expects the repo mounted at /src, --privileged, --network host, and the
# host's /lib/modules so modprobe can load rdma_rxe into the host kernel.
set -euo pipefail

CONF=/src/scripts/softroce/ip_local.conf
PORT=21211
BUILD=/tmp/cotra-build
BIN="${BUILD}/tests/rdma/test_read"
IB_DEV=${IB_DEV:-rxe0}

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
  local netdev=$1
  rdma link del rxe0 2>/dev/null || true
  if [[ "${netdev}" == "dummy0" ]]; then
    ip link add dummy0 type dummy 2>/dev/null || true
    ip link set dummy0 up
    ip addr replace 192.168.100.1/24 dev dummy0
  fi
  rdma link add rxe0 type rxe netdev "${netdev}"
  rdma link show
}

run_pair() {
  local log=/tmp/test_read.log
  if [[ "${IB_DEV}" != shm* ]]; then
    setup_memcached
  fi
  # 2560 * 200000 = 512MB. A 2.5GB registration is rejected by rxe here.
  # timeout: a lost READ completion otherwise spins in poll_read.
  stdbuf -oL -eL "${BIN}" -m 1 -t 1 -n 200000 -d "${IB_DEV}" --config_file "${CONF}" \
    >/tmp/follower.log 2>&1 &
  local follower=$!
  set +e
  stdbuf -oL -eL timeout --foreground 120 \
    "${BIN}" -m 0 -t 1 -n 200000 -d "${IB_DEV}" --config_file "${CONF}" >"${log}" 2>&1
  local status=$?
  set -e
  kill "${follower}" 2>/dev/null || true
  wait "${follower}" 2>/dev/null || true
  echo "----- leader -----"
  cat "${log}"
  echo "----- follower (tail) -----"
  tail -n 50 /tmp/follower.log || true
  if [[ ${status} -ne 0 ]]; then
    return "${status}"
  fi
  grep -q 'read-268-B' "${log}"
}

if [[ "${IB_DEV}" != shm* ]]; then
  modprobe rdma_rxe
fi
g++ --version | head -n 1
cmake -S /src -B "${BUILD}" -DCOTRA_WITH_INDEX=OFF -DMACHINE_NUM=2
cmake --build "${BUILD}" -j "$(nproc)" --target test_read

if [[ "${IB_DEV}" == shm* ]]; then
  if run_pair; then
    echo "shm test_read passed"
    exit 0
  fi
  echo "shm test_read failed"
  exit 1
fi

if link_rxe lo && run_pair; then
  echo "soft-roce test_read passed on lo"
  exit 0
fi

echo "lo path failed, retrying with dummy0"
if link_rxe dummy0 && run_pair; then
  echo "soft-roce test_read passed on dummy0"
  exit 0
fi

echo "soft-roce test_read failed"
exit 1
