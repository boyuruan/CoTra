#!/bin/bash
# Compile and run test_read inside the Soft-RoCE image.
# The host needs Docker and the rdma_rxe module. It does not need a compiler,
# libibverbs headers, or memcached installed.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)

docker build -t cotra-softroce \
  -f "${ROOT}/scripts/softroce/Dockerfile" \
  "${ROOT}/scripts/softroce"

docker run --rm --privileged --network host --ipc=host \
  --ulimit memlock=-1:-1 \
  -e "IB_DEV=${IB_DEV:-rxe0}" \
  -v /lib/modules:/lib/modules:ro \
  -v "${ROOT}":/src:ro \
  cotra-softroce \
  bash /src/scripts/softroce/inside.sh
