#!/usr/bin/env bash
set -euo pipefail

version="${BORE_VERSION:-v0.6.0}"
dest="${1:-${RUNNER_TEMP:-/tmp}/bore}"

case "$(uname -m)" in
  x86_64|amd64)
    target="x86_64-unknown-linux-musl"
    expected="e484d1e3acba77169b773f31a5bfb34192d4b660f44a094a658a2522cd2270f7"
    ;;
  aarch64|arm64)
    target="aarch64-unknown-linux-musl"
    expected="ffc4515f3617420b243758cf36ed6a63208d7dba76b2ec3e90d1f476a9742951"
    ;;
  armv7l|armv7)
    target="armv7-unknown-linux-musleabihf"
    expected="d8c93e3c3d3da043bb9aa19fee1359f2160f5e9d4a146d69995e97307ce9067e"
    ;;
  *)
    echo "Unsupported architecture for bore: $(uname -m)" >&2
    exit 1
    ;;
esac

url="https://github.com/ekzhang/bore/releases/download/${version}/bore-${version}-${target}.tar.gz"
archive="${RUNNER_TEMP:-/tmp}/bore-${version}-${target}.tar.gz"

mkdir -p "$dest"
curl -fsSL --retry 5 --retry-delay 2 -o "$archive" "$url"
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
tar -xzf "$archive" -C "$dest"
rm -f "$archive"
chmod +x "$dest/bore"

echo "BORE_BIN=$dest/bore" >> "${GITHUB_ENV:-/dev/null}"

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  echo "bore_bin=$dest/bore" >> "$GITHUB_OUTPUT"
fi

"$dest/bore" --version
