#!/bin/sh
# fetch.sh <url> <out> [sha1]: resumable download. Proxies with TLS interception may close long
# transfers early; each attempt continues where the last one stopped (curl -C -).
set -eu
url=$1 out=$2 sum=${3:-}
total=$(curl --http1.1 -fsSIL "$url" | tr -d '\r' | awk 'tolower($1)=="content-length:"{n=$2} END{print n}')
i=0
while [ "$(stat -c %s "$out" 2>/dev/null || echo 0)" -lt "$total" ]; do
  i=$((i + 1)); [ "$i" -le 100 ] || { echo "fetch: gave up after $i attempts" >&2; exit 1; }
  curl --http1.1 -fsS -C - -o "$out" "$url" || true
done
echo "fetch: $(stat -c %s "$out") bytes in $i attempts"
[ -z "$sum" ] || echo "$sum  $out" | sha1sum -c -
