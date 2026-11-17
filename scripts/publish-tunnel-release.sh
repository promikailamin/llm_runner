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
notes="$work_dir/notes.md"

python3 - "$tunnel_file" "$notes" <<'PY'
import datetime
import json
import sys

tunnel_file, notes_file = sys.argv[1:]
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
status = data.get("status", "online")
local_port = data.get("local_port", "")

def code(value):
    return f"`{value}`" if value else "not available"

lines = [
    "# LLM tunnel",
    "",
    "This release is recreated for every **LLM over bore.pub** workflow run and is",
    "deleted when the run finishes, times out, or is cancelled.",
    "",
    "## Connection",
    "",
    f"- Status: **{status}**",
    f"- Address: {code(address)}",
    f"- API base URL: {code(url)}",
    f"- Model: {code(model)}",
    f"- Local Ollama port: {code(local_port)}",
    "",
    "## Action",
    "",
    f"- Action ID: {code(run_id)}",
    f"- Run attempt: {code(run_attempt)}",
    f"- Action URL: {run_url or 'not available'}",
    f"- Run started (UTC): {code(started)}",
    f"- Timeout: {timeout} minute(s)" if timeout else "- Timeout: not available",
    f"- Expires (UTC): {code(expires)}",
    "",
    "## Usage",
    "",
    "```bash",
    f"curl {url}/api/tags",
    f"curl {url}/api/generate \\",
    f"  -d '{{\"model\":\"{model}\",\"prompt\":\"Say hello\",\"stream\":false}}'",
    "```",
    "",
    "```bash",
    "llm-agent \\",
    f"  --url {url} \\",
    f"  --model '{model}' \\",
    "  --task 'Inspect the repository and run the tests'",
    "```",
    "",
    "The bore.pub tunnel is public and Ollama has no authentication. Anyone with",
    "the address can use the model until the run ends.",
    "",
]

with open(notes_file, "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines))
PY

if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
  echo "Deleting previous release $tag"
  gh release delete "$tag" --repo "$repo" --yes --cleanup-tag
fi

gh release create "$tag" "$tunnel_file" \
  --repo "$repo" \
  --title "LLM tunnel" \
  --notes-file "$notes" \
  --latest=false

echo "Published tunnel release $tag for $repo"
