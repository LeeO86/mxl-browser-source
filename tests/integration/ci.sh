#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The integration tests of SPEC §17 against an image, in software mode on a tmpfs MXL root (CI):
# interaction and IME, dialogs/popups/downloads/permissions, templates/crash/hang, IS-05 enable
# and restart. latency_test.py stays a lab test (its 150 ms budget needs the lab host's clock and
# CPU). The container log goes to $IT_ARTIFACTS (default ./it-artifacts).
#
#   tests/integration/ci.sh <image>
set -euo pipefail

IMAGE="${1:?usage: ci.sh <image>}"
ART="${IT_ARTIFACTS:-$PWD/it-artifacts}"
NAME=mbs-it
DOMAIN=/Volumes/mxl/browser-source-it
mkdir -p "$ART"
trap 'docker logs "$NAME" >"$ART/$NAME.log" 2>&1 || true; docker rm -f "$NAME" >/dev/null 2>&1 || true' EXIT

HOST_IP="$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')"
docker run -d --name "$NAME" --network host --shm-size 1g \
    --tmpfs /Volumes/mxl:rw,size=1g,mode=1777 \
    -e BROWSER_RENDER=software -e NMOS_HOST_ADDRESS="$HOST_IP" -e NMOS_SEED=it \
    -e MXL_OUTPUT_DOMAIN_DIR="$DOMAIN" -e LOG_FORMAT=text "$IMAGE" >/dev/null
for _ in $(seq 1 60); do
    curl -fs -o /dev/null http://127.0.0.1:8160/readyz && break
    sleep 1
done
curl -fs -o /dev/null http://127.0.0.1:8160/readyz || { echo "browser source not ready" >&2; exit 1; }

cd "$(dirname "$0")"
export DOCKER=docker
python3 interact_test.py 127.0.0.1:8160
python3 pages_test.py 127.0.0.1:8160 "$NAME"
python3 ops_test.py 127.0.0.1:8160 "$NAME"
python3 nmos_test.py 127.0.0.1:8160 "$NAME" "$DOMAIN" 3312
echo "integration tests passed"
