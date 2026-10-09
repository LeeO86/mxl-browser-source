#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The integration tests of SPEC §17 against an image, in software mode on a tmpfs MXL root (CI):
# interaction and IME, dialogs/popups/downloads/permissions, templates/crash/hang, IS-05 enable
# and restart, then the lifecycle of guideline G14: registered → SIGTERM → exit 143, node gone
# from the Query API, own domain removed. A registry runs next to it (the platform's image with
# docker/registry.json). latency_test.py stays a lab test (its 150 ms budget needs the lab host's
# clock and CPU). The container logs go to $IT_ARTIFACTS (default ./it-artifacts).
#
#   tests/integration/ci.sh <image>
set -euo pipefail

IMAGE="${1:?usage: ci.sh <image>}"
ART="${IT_ARTIFACTS:-$PWD/it-artifacts}"
NAME=mbs-it
DOMAIN=/Volumes/mxl/browser-source-it
REGISTRY=docker.io/gemini2350/nmos-cpp-registry:sha-53481ff@sha256:3c2fa0793ce336e4176487878758efcbd5c61414c5b90433f3758851615f1c91
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$ART"
cleanup() {
    for c in "$NAME" mbs-it-registry; do
        docker logs "$c" >"$ART/$c.log" 2>&1 || true
        docker rm -f "$c" >/dev/null 2>&1 || true
    done
}
trap cleanup EXIT

HOST_IP="$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')"
docker run -d --name mbs-it-registry --network host -e RUN_MQTT=FALSE -e ADVERTISE_MQTT=FALSE \
    -v "$HERE/../../docker/registry.json:/home/registry.json:ro" "$REGISTRY" >/dev/null
docker run -d --name "$NAME" --network host --shm-size 1g \
    --tmpfs /Volumes/mxl:rw,size=1g,mode=1777 \
    -e BROWSER_RENDER=software -e NMOS_HOST_ADDRESS="$HOST_IP" -e NMOS_SEED=it \
    -e NMOS_REGISTRY_ADDRESS="$HOST_IP" -e NMOS_REGISTRY_PORT=3210 -e MXL_CLEANUP_ON_EXIT=true \
    -e MXL_OUTPUT_DOMAIN_DIR="$DOMAIN" -e LOG_FORMAT=text "$IMAGE" >/dev/null
# /readyz needs the node in the registry's Query API.
for _ in $(seq 1 90); do
    curl -fs -o /dev/null http://127.0.0.1:8160/readyz && break
    sleep 1
done
curl -fs -o /dev/null http://127.0.0.1:8160/readyz || { echo "browser source not ready" >&2; exit 1; }

cd "$HERE"
export DOCKER=docker
python3 interact_test.py 127.0.0.1:8160 "$NAME" "$DOMAIN"
python3 pages_test.py 127.0.0.1:8160 "$NAME"
python3 ops_test.py 127.0.0.1:8160 "$NAME"
python3 nmos_test.py 127.0.0.1:8160 "$NAME" "$DOMAIN" 3312

# G14 lifecycle. nmos_test.py restarted the container: wait until it is registered again.
for _ in $(seq 1 60); do
    curl -fs -o /dev/null http://127.0.0.1:8160/readyz && break
    sleep 1
done
NODE="$(curl -fs http://127.0.0.1:8160/api/v1/nmos | python3 -c 'import json, sys; print(json.load(sys.stdin)["node_id"])')"
curl -fs -o /dev/null "http://127.0.0.1:3211/x-nmos/query/v1.3/nodes/$NODE" || { echo "node not in the registry" >&2; exit 1; }
since="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
docker stop -t 20 "$NAME" >/dev/null
code="$(docker inspect -f '{{.State.ExitCode}}' "$NAME")"
[[ "$code" == 143 ]] || { echo "SIGTERM: exit $code, expected 143" >&2; exit 1; }
docker logs --since "$since" "$NAME" 2>&1 | grep -q domain_removed || { echo "SIGTERM: own domain not removed" >&2; exit 1; }
status="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:3211/x-nmos/query/v1.3/nodes/$NODE")"
[[ "$status" == 404 ]] || { echo "SIGTERM: node still in the registry ($status)" >&2; exit 1; }
echo "lifecycle: registered, SIGTERM -> exit 143, node removed from the registry, own domain removed"
echo "integration tests passed"
