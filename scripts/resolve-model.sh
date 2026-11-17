#!/usr/bin/env bash
set -euo pipefail

model="${1:-${INPUT_MODEL:-}}"
custom="${2:-${INPUT_CUSTOM_MODEL:-}}"

if [ "$model" = "custom" ]; then
  model="$custom"
fi

if [ -z "$model" ]; then
  echo "No model selected. Choose a model or provide custom_model." >&2
  exit 2
fi

if ! printf '%s' "$model" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._:/-]*$'; then
  echo "Invalid model reference: $model" >&2
  exit 2
fi

safe="$(printf '%s' "$model" | sed 's#[:/]#-#g; s#[^A-Za-z0-9._-]#-#g')"

echo "MODEL=$model" >> "${GITHUB_ENV:-/dev/null}"
echo "MODEL_SAFE=$safe" >> "${GITHUB_ENV:-/dev/null}"

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  {
    echo "model=$model"
    echo "safe=$safe"
  } >> "$GITHUB_OUTPUT"
fi

echo "$model"
