#!/bin/bash
# Compile and run the synthetic scalagraph_v3 / scala_v3 path in Docker.
# The host needs Docker and the rdma_rxe module.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)

docker build -t cotra-softroce \
  -f "${ROOT}/scripts/softroce/Dockerfile" \
  "${ROOT}/scripts/softroce"

docker run --rm --privileged --network host \
  --ulimit memlock=-1:-1 \
  -v /lib/modules:/lib/modules:ro \
  -v "${ROOT}":/src:ro \
  cotra-softroce \
  bash /src/scripts/softroce/e2e.sh
