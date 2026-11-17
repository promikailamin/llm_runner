#!/usr/bin/env bash
set -euo pipefail

minutes="${1:-30}"
shift || true

duration=$((minutes * 60))
start=$SECONDS
interval="${KEEPALIVE_INTERVAL_SECONDS:-30}"

echo "Keeping the LLM service alive for ${minutes} minute(s)"

while [ $((SECONDS - start)) -lt "$duration" ]; do
  for pid in "$@"; do
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
      echo "Process ${pid} is no longer running" >&2
      exit 1
    fi
  done

  if ! curl -fsS --max-time 10 http://127.0.0.1:11434/api/tags >/dev/null 2>&1; then
    echo "Ollama health check failed" >&2
    exit 1
  fi

  remaining=$((duration - (SECONDS - start)))
  echo "alive: ${remaining}s remaining"
  sleep "$interval"
done

echo "Keep-alive window finished"
