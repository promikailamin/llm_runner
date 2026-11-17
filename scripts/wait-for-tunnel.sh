#!/usr/bin/env bash
set -euo pipefail

repo="${1:?repository is required}"
run_id="${2:?run id is required}"
artifact="${3:?artifact name is required}"
out_dir="${4:-tunnel-info}"
max_wait_seconds="${5:-3600}"
poll_seconds="${6:-10}"

mkdir -p "$out_dir"
deadline=$((SECONDS + max_wait_seconds))
tunnel_file=""

while [ "$SECONDS" -lt "$deadline" ]; do
  count="$(gh api "repos/${repo}/actions/runs/${run_id}/artifacts?name=${artifact}" --jq '.total_count' 2>/dev/null || echo 0)"
  if [ "${count:-0}" -gt 0 ]; then
    rm -rf "${out_dir}/download"
    if gh run download "$run_id" --repo "$repo" --name "$artifact" --dir "${out_dir}/download" >/dev/null 2>&1; then
      if [ -f "${out_dir}/download/tunnel.json" ]; then
        tunnel_file="${out_dir}/download/tunnel.json"
        cp "$tunnel_file" "${out_dir}/tunnel.json"
        tunnel_file="${out_dir}/tunnel.json"
        break
      fi
    fi
  fi
  sleep "$poll_seconds"
done

if [ -z "$tunnel_file" ]; then
  echo "Timed out waiting for artifact ${artifact}" >&2
  exit 1
fi

python3 - "$tunnel_file" <<'PY'
import json
import os
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)

address = data["address"]
model = data.get("model", "")
print(f"Tunnel address: {address}")
print(f"Model: {model}")

env_file = os.environ.get("GITHUB_ENV")
if env_file:
    with open(env_file, "a", encoding="utf-8") as handle:
        handle.write(f"TUNNEL_ADDRESS={address}\n")
        handle.write(f"TUNNEL_HOST={data.get('host', 'bore.pub')}\n")
        handle.write(f"TUNNEL_PORT={data.get('port', '')}\n")
        handle.write(f"TUNNEL_URL={data.get('url', 'http://' + address)}\n")
        handle.write(f"TUNNEL_MODEL={model}\n")

output_file = os.environ.get("GITHUB_OUTPUT")
if output_file:
    with open(output_file, "a", encoding="utf-8") as handle:
        handle.write(f"address={address}\n")
        handle.write(f"host={data.get('host', 'bore.pub')}\n")
        handle.write(f"port={data.get('port', '')}\n")
        handle.write(f"url={data.get('url', 'http://' + address)}\n")
        handle.write(f"model={model}\n")
        handle.write(f"json={sys.argv[1]}\n")
PY
