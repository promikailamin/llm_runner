#!/usr/bin/env bash
set -euo pipefail

tag="${1:-tunnel}"
repo="${2:-${GITHUB_REPOSITORY:-}}"

if [ -z "$repo" ]; then
  echo "repository is required" >&2
  exit 1
fi

if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
  gh release delete "$tag" --repo "$repo" --yes --cleanup-tag
  echo "Deleted tunnel release $tag from $repo"
else
  echo "No tunnel release $tag to delete in $repo"
fi
