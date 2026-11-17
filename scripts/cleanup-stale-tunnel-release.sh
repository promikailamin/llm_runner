#!/usr/bin/env bash
set -euo pipefail

repo="${1:-${GITHUB_REPOSITORY:-}}"
workflow_file="${2:-llm.yml}"
tag="${3:-tunnel}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "$repo" ]; then
  echo "repository is required" >&2
  exit 1
fi

active="$(
  gh api "repos/${repo}/actions/workflows/${workflow_file}/runs?per_page=30" \
    --jq '[.workflow_runs[] | select(.status == "in_progress" or .status == "queued" or .status == "waiting" or .status == "requested" or .status == "pending")] | length'
)"

if [ "${active:-0}" -gt 0 ]; then
  echo "Found ${active} active or pending ${workflow_file} run(s); keeping release ${tag}"
  exit 0
fi

bash "${script_dir}/delete-tunnel-release.sh" "$tag" "$repo"
