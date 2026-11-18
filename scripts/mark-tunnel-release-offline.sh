#!/usr/bin/env bash
set -euo pipefail

tag="${1:-tunnel}"
repo="${2:-${GITHUB_REPOSITORY:-}}"
reason="${3:-run ended}"

if [ -z "$repo" ]; then
  echo "repository is required" >&2
  exit 1
fi

if ! gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
  echo "No tunnel release $tag to mark offline in $repo"
  exit 0
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
existing="$work_dir/body.json"
notes="$work_dir/notes.json"

gh release view "$tag" --repo "$repo" --json body --jq '.body // ""' > "$existing"

python3 - "$existing" "$notes" "$reason" <<'PY'
import datetime
import json
import sys

existing_file, notes_file, reason = sys.argv[1:]
try:
    with open(existing_file, encoding="utf-8") as handle:
        body = json.load(handle)
    if not isinstance(body, dict):
        body = {"previous_body": body}
except (OSError, ValueError):
    body = {}

body["schema_version"] = body.get("schema_version", 1)
body["title"] = body.get("title", "LLM tunnel")
body["status"] = "offline"
body["ended_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
body["offline_reason"] = reason
body["lifecycle"] = (
    "This release is kept after the run ends. An offline status means the "
    "tunnel is no longer available; a later run updates the same release and "
    "sets the status back to online."
)

with open(notes_file, "w", encoding="utf-8") as handle:
    json.dump(body, handle, indent=2)
    handle.write("\n")
PY

gh release edit "$tag" \
  --repo "$repo" \
  --title "LLM tunnel" \
  --notes-file "$notes"

echo "Marked tunnel release $tag offline in $repo"
