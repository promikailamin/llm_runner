#!/usr/bin/env bash
set -euo pipefail

local_port="${1:-11434}"
output_file="${2:-tunnel.json}"
model="${3:-unknown}"
timeout_minutes="${4:-30}"
bore_bin="${BORE_BIN:-bore}"
max_attempts="${5:-5}"

out_dir="$(dirname "$output_file")"
mkdir -p "$out_dir"
log_file="${out_dir}/bore.log"
pid_file="${out_dir}/bore.pid"
: > "$log_file"

address=""
pid=""

cleanup_failed_attempt() {
  local candidate_pid="$1"
  if [ -n "$candidate_pid" ] && kill -0 "$candidate_pid" 2>/dev/null; then
    kill "$candidate_pid" 2>/dev/null || true
    wait "$candidate_pid" 2>/dev/null || true
  fi
}

for attempt in $(seq 1 "$max_attempts"); do
  : > "$log_file"
  NO_COLOR=1 nohup "$bore_bin" local "$local_port" --to bore.pub >"$log_file" 2>&1 &
  pid=$!
  disown "$pid" 2>/dev/null || true
  echo "$pid" > "$pid_file"

  for _ in $(seq 1 120); do
    if ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    candidate="$(grep -oE 'bore[.]pub:[0-9]+' "$log_file" | head -n1 || true)"
    if [ -n "$candidate" ]; then
      address="$candidate"
      break
    fi
    sleep 1
  done

  if [ -n "$address" ]; then
    break
  fi

  echo "bore tunnel attempt ${attempt}/${max_attempts} failed" >&2
  cleanup_failed_attempt "$pid"
  pid=""
  sleep 2
done

if [ -z "$address" ] || [ -z "$pid" ]; then
  echo "Could not establish a bore.pub tunnel" >&2
  cat "$log_file" >&2 || true
  exit 1
fi

port="${address##*:}"

python3 - "$output_file" "$address" "$port" "$local_port" "$model" "$timeout_minutes" "$pid" <<'PY'
import datetime
import json
import os
import sys

output_file, address, port, local_port, model, timeout_minutes, pid = sys.argv[1:]
data = {
    "host": "bore.pub",
    "port": int(port),
    "address": address,
    "url": f"http://{address}",
    "local_port": int(local_port),
    "model": model,
    "timeout_minutes": int(float(timeout_minutes)),
    "pid": int(pid),
    "created_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "repository": os.environ.get("GITHUB_REPOSITORY", ""),
    "run_id": os.environ.get("GITHUB_RUN_ID", ""),
    "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT", ""),
    "run_url": (
        f"{os.environ.get('GITHUB_SERVER_URL', 'https://github.com')}/"
        f"{os.environ.get('GITHUB_REPOSITORY', '')}/actions/runs/"
        f"{os.environ.get('GITHUB_RUN_ID', '')}"
    ),
}
with open(output_file, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2)
    handle.write("\n")
PY

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  {
    echo "address=$address"
    echo "host=bore.pub"
    echo "port=$port"
    echo "pid=$pid"
    echo "json=$output_file"
  } >> "$GITHUB_OUTPUT"
fi

echo "Tunnel ready at ${address} (local port ${local_port}, pid ${pid})"
