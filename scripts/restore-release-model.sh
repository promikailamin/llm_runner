#!/usr/bin/env bash
set -euo pipefail

safe_name="${1:?safe model name is required}"
tag="${2:?release tag is required}"
repo="${3:?repository is required}"
dest="${4:?destination models directory is required}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$dest"

gh release download "$tag" \
  --repo "$repo" \
  --dir "$tmp" \
  --clobber \
  --pattern "${safe_name}.tar.part-*" \
  --pattern "SHA256SUMS"

mapfile -t parts < <(find "$tmp" -maxdepth 1 -type f -name "${safe_name}.tar.part-*" | LC_ALL=C sort)
if [ "${#parts[@]}" -eq 0 ]; then
  echo "No release assets found for ${safe_name} in ${tag}" >&2
  exit 1
fi

if [ -f "$tmp/SHA256SUMS" ]; then
  grep -E "  ${safe_name//./\\.}\.tar\.part-" "$tmp/SHA256SUMS" > "$tmp/selected.sha256" || true
  if [ -s "$tmp/selected.sha256" ]; then
    ( cd "$tmp" && sha256sum -c selected.sha256 )
  fi
fi

cat "${parts[@]}" | tar -xf - -C "$dest"
echo "Restored ${safe_name} from release ${tag} into ${dest}"
