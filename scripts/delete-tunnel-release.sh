#!/usr/bin/env bash
set -euo pipefail

tag="${1:-tunnel}"
repo="${2:-${GITHUB_REPOSITORY:-}}"

if [ -z "$repo" ]; then
  echo "repository is required" >&2
  exit 1
fi

found=0
if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
  gh release delete "$tag" --repo "$repo" --yes --cleanup-tag
  found=1
fi

if gh api "repos/${repo}/git/refs/tags/${tag}" >/dev/null 2>&1; then
  gh api -X DELETE "repos/${repo}/git/refs/tags/${tag}" >/dev/null
  found=1
fi

if [ "$found" -eq 1 ]; then
  echo "Deleted tunnel release and tag $tag from $repo"
else
  echo "No tunnel release or tag $tag to delete in $repo"
fi
