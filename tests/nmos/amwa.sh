#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# AMWA nmos-testing against the browser source (SPEC §17, acceptance criterion 15):
# IS-04-01, IS-05-01 (v1.2), IS-05-02 and BCP-007-03-01, non-interactively.
#
#   tests/nmos/amwa.sh <image>
#
# The tool's mock registry is announced over multicast DNS-SD, so the container runs with
# NMOS_DNS_SD=true and talks to the host's avahi-daemon over D-Bus. Run it on an isolated host
# (CI runner), not in a shared lab network: other NMOS nodes there could register with the mock
# registry. Software rendering, the MXL root on a tmpfs. Any "Fail" or "Test Error" fails the run;
# JSON results and logs go to $AMWA_ARTIFACTS (default ./amwa-artifacts).
set -euo pipefail

IMAGE="${1:?usage: amwa.sh <image>}"
# The pin lives in .github/workflows/ci.yaml.
REF="${NMOS_TESTING_REF:-$(sed -n 's/^ *NMOS_TESTING_REF: *\([0-9a-f]\{40\}\).*/\1/p' "$(dirname "$0")/../../.github/workflows/ci.yaml" | head -1)}"
[[ -n "$REF" ]] || { echo "NMOS_TESTING_REF not set and not found in ci.yaml" >&2; exit 2; }
ART="${AMWA_ARTIFACTS:-$PWD/amwa-artifacts}"
NAME=mbs-amwa
WEB=18160
PORT=18312
mkdir -p "$ART"
WORK="$(mktemp -d)"
cleanup() {
    docker logs "$NAME" >"$ART/$NAME.log" 2>&1 || true
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# avahi on the host (nmos-cpp's DNS-SD client in the container talks to it over D-Bus)
if ! pgrep -x avahi-daemon >/dev/null; then
    command -v avahi-daemon >/dev/null || sudo apt-get install -y -qq avahi-daemon >/dev/null
    pgrep -x avahi-daemon >/dev/null || sudo systemctl start avahi-daemon
fi

# nmos-testing at the pinned commit
TOOL="$WORK/nmos-testing"
mkdir -p "$TOOL"
curl -fsSL "https://codeload.github.com/AMWA-TV/nmos-testing/tar.gz/$REF" | tar xz -C "$TOOL" --strip-components=1
python3 -m venv "$WORK/venv"
"$WORK/venv/bin/pip" install -q --disable-pip-version-check -r "$TOOL/requirements.txt"
cat >"$TOOL/nmostesting/UserConfig.py" <<'EOF'
from . import Config as CONFIG
CONFIG.ENABLE_DNS_SD = True
CONFIG.DNS_SD_MODE = 'multicast'
CONFIG.DNS_SD_ADVERT_TIMEOUT = 60
CONFIG.API_PROCESSING_TIMEOUT = 2
CONFIG.HTTP_TIMEOUT = 5
CONFIG.MAX_TEST_ITERATIONS = 0
EOF

# the browser source
HOST_IP="$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')"
aa=()
# dbus-daemon refuses AppArmor-confined clients without D-Bus rules (docker-default)
[[ "$(cat /sys/module/apparmor/parameters/enabled 2>/dev/null)" == "Y" ]] && aa=(--security-opt apparmor=unconfined)
docker run -d --name "$NAME" --network host --shm-size 1g "${aa[@]}" \
    -v /run/dbus:/run/dbus -v /run/avahi-daemon:/run/avahi-daemon \
    --tmpfs /Volumes/mxl:rw,size=512m,mode=1777 \
    -e BROWSER_RENDER=software -e NMOS_DNS_SD=true -e NMOS_HOST_ADDRESS="$HOST_IP" \
    -e NMOS_SEED=amwa -e WEB_PORT="$WEB" -e NMOS_PORT="$PORT" -e LOG_FORMAT=text "$IMAGE" >/dev/null
for _ in $(seq 1 60); do
    curl -fs -o /dev/null "http://127.0.0.1:$WEB/livez" && break
    sleep 1
done
curl -fs -o /dev/null "http://127.0.0.1:$WEB/livez" || { echo "browser source not live" >&2; exit 1; }

failed=()
run_suite() { # <tag> <suite> <args...>
    local tag="$1" suite="$2"
    shift 2
    echo "== $tag"
    set +e
    (cd "$TOOL" && "$WORK/venv/bin/python" nmos-test.py suite "$suite" --selection all "$@" --output "$ART/$tag.json") >"$ART/$tag.log" 2>&1
    local rc=$?
    python3 - "$ART/$tag.json" <<'EOF'
import json, sys
try:
    data = json.load(open(sys.argv[1]))
except Exception as e:
    print(f"  (no results: {e})")
    sys.exit(3)
counts = {}
for r in data.get("results", []):
    counts[r["state"]] = counts.get(r["state"], 0) + 1
    if r["state"] in ("Fail", "Test Error"):
        print(f"  {r['state']}: {r['name']}: {r.get('detail', '')}")
print("  " + ", ".join(f"{k}: {v}" for k, v in sorted(counts.items())))
sys.exit(3 if counts.get("Fail") or counts.get("Test Error") else 0)
EOF
    local found=$?
    set -e
    # nmos-test.py: 0 all passed, 1 warnings or could-not-test, anything else a failure
    [[ $rc -le 1 && $found -eq 0 ]] || failed+=("$tag")
}

# IS-05 v1.2 only: the MXL transport exists from v1.2 on.
run_suite IS-04-01 IS-04-01 --host "$HOST_IP" --port "$PORT" --version v1.3
run_suite IS-05-01 IS-05-01 --host "$HOST_IP" --port "$PORT" --version v1.2
run_suite IS-05-02 IS-05-02 --host "$HOST_IP" "$HOST_IP" --port "$PORT" "$PORT" --version v1.3 v1.2
run_suite BCP-007-03-01 BCP-007-03-01 --host "$HOST_IP" "$HOST_IP" --port "$PORT" "$PORT" --version v1.3 v1.2

if ((${#failed[@]})); then
    echo "suites with failures: ${failed[*]}" >&2
    exit 1
fi
echo "AMWA: all suites without failures"
