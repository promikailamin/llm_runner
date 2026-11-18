#!/usr/bin/env bash
set -euo pipefail

tunnel_file="${1:?tunnel.json path is required}"
tag="${2:-tunnel}"
repo="${3:-${GITHUB_REPOSITORY:-}}"

if [ -z "$repo" ]; then
  echo "repository is required" >&2
  exit 1
fi
if [ ! -f "$tunnel_file" ]; then
  echo "tunnel file not found: $tunnel_file" >&2
  exit 1
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
notes="$work_dir/notes.json"

python3 - "$tunnel_file" "$notes" "$repo" <<'PY'
import datetime
import json
import sys

tunnel_file, notes_file, repo = sys.argv[1:]
with open(tunnel_file, encoding="utf-8") as handle:
    data = json.load(handle)

address = data.get("address", "")
url = data.get("url") or f"http://{address}"
model = data.get("model", "unknown")
run_id = data.get("run_id") or data.get("action_id") or ""
run_attempt = data.get("run_attempt", "")
run_url = data.get("run_url", "")
started = data.get("run_started_at") or data.get("created_at", "")
expires = data.get("expires_at", "")
timeout = data.get("timeout_minutes", "")
local_port = data.get("local_port", "")
status = data.get("status", "online")
server = data.get("server_url") or "https://github.com"
repository = data.get("repository") or repo
if not run_url and run_id and repository:
    run_url = f"{server}/{repository}/actions/runs/{run_id}"

now = datetime.datetime.now(datetime.timezone.utc).isoformat()

body = {
    "schema_version": 1,
    "status": status,
    "title": "LLM tunnel",
    "description": (
        "Live Ollama model exposed through a public bore.pub tunnel. This "
        "release is kept after the run ends and is marked offline instead of "
        "being deleted."
    ),
    "address": address,
    "url": url,
    "host": data.get("host", "bore.pub"),
    "port": data.get("port", ""),
    "local_port": local_port,
    "model": model,
    "repository": repository,
    "run": {
        "id": run_id,
        "action_id": data.get("action_id", run_id),
        "attempt": run_attempt,
        "url": run_url,
        "started_at": started,
        "timeout_minutes": timeout,
        "expires_at": expires,
    },
    "published_at": now,
    "usage": {
        "api_base_url": url,
        "tags": f"curl {url}/api/tags",
        "generate": (
            f"curl {url}/api/generate -d "
            f"'{{\"model\":\"{model}\",\"prompt\":\"Say hello\",\"stream\":false}}'"
        ),
        "agent": (
            f"llm-agent --url {url} --model '{model}' "
            "--task 'Inspect the repository and run the tests'"
        ),
    },
    "security": (
        "The bore.pub tunnel is public and Ollama has no authentication. "
        "Anyone with the address can use the model while the status is online."
    ),
    "lifecycle": (
        "When the run ends, times out, or is cancelled, the status changes to "
        "offline and the release is kept."
    ),
}

with open(notes_file, "w", encoding="utf-8") as handle:
    json.dump(body, handle, indent=2)
    handle.write("\n")
PY

if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
  gh release edit "$tag" \
    --repo "$repo" \
    --title "LLM tunnel" \
    --notes-file "$notes"
  gh release upload "$tag" "$tunnel_file" --repo "$repo" --clobber
  echo "Updated tunnel release $tag for $repo"
else
  gh release create "$tag" "$tunnel_file" \
    --repo "$repo" \
    --title "LLM tunnel" \
    --notes-file "$notes" \
    --latest=false
  echo "Published tunnel release $tag for $repo"
fi
