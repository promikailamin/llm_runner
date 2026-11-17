#!/usr/bin/env bash
set -euo pipefail

address="${1:?tunnel address is required}"
minutes="${2:-1}"
interval="${3:-30}"

duration=$((minutes * 60))
start=$SECONDS
ok=0
failed=0

echo "Probing http://${address}/api/tags for ${minutes} minute(s)"

while [ $((SECONDS - start)) -lt "$duration" ]; do
  if curl -fsS --max-time 20 "http://${address}/api/tags" >/dev/null 2>&1; then
    ok=$((ok + 1))
    echo "ok: probe ${ok} via ${address}"
  else
    failed=$((failed + 1))
    echo "failed: probe ${failed} via ${address}" >&2
  fi

  remaining=$((duration - (SECONDS - start)))
  if [ "$remaining" -gt 0 ]; then
    pause="$interval"
    if [ "$remaining" -lt "$pause" ]; then
      pause="$remaining"
    fi
    sleep "$pause"
  fi
done

echo "Probe summary: ${ok} succeeded, ${failed} failed"

if [ "$failed" -gt 0 ]; then
  exit 1
fi
