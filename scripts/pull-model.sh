#!/usr/bin/env bash
set -euo pipefail

model="${1:?model is required}"
attempts="${2:-3}"

for attempt in $(seq 1 "$attempts"); do
  echo "Pulling ${model} (attempt ${attempt}/${attempts})"
  if OLLAMA_HOST="${OLLAMA_HOST:-127.0.0.1:11434}" ollama pull "$model"; then
    exit 0
  fi
  sleep 5
done

echo "Failed to pull ${model}" >&2
exit 1
