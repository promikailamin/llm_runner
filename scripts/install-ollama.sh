#!/usr/bin/env bash
set -euo pipefail

models_dir="${OLLAMA_MODELS:-$HOME/.ollama/models}"
log_file="${RUNNER_TEMP:-/tmp}/ollama.log"
pid_file="${RUNNER_TEMP:-/tmp}/ollama.pid"

if ! command -v ollama >/dev/null 2>&1; then
  curl -fsSL https://ollama.com/install.sh | sh
fi

sudo systemctl stop ollama >/dev/null 2>&1 || true
sudo systemctl disable ollama >/dev/null 2>&1 || true
sudo pkill -f 'ollama serve' >/dev/null 2>&1 || true

export OLLAMA_MODELS="$models_dir"
mkdir -p "$models_dir"
: > "$log_file"

nohup env \
  OLLAMA_HOST=0.0.0.0:11434 \
  OLLAMA_MODELS="$models_dir" \
  OLLAMA_NUM_PARALLEL=1 \
  OLLAMA_MAX_LOADED_MODELS=1 \
  ollama serve >"$log_file" 2>&1 &
pid=$!
disown "$pid" 2>/dev/null || true
echo "$pid" > "$pid_file"

for _ in $(seq 1 180); do
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "ollama serve exited early" >&2
    cat "$log_file" >&2 || true
    exit 1
  fi
  if curl -fsS --max-time 2 http://127.0.0.1:11434/api/tags >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

if ! curl -fsS --max-time 5 http://127.0.0.1:11434/api/tags >/dev/null 2>&1; then
  echo "ollama API did not become ready" >&2
  cat "$log_file" >&2 || true
  exit 1
fi

{
  echo "OLLAMA_MODELS=$models_dir"
  echo "OLLAMA_PID=$pid"
  echo "OLLAMA_HOST=http://127.0.0.1:11434"
} >> "${GITHUB_ENV:-/dev/null}"

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  {
    echo "pid=$pid"
    echo "models_dir=$models_dir"
  } >> "$GITHUB_OUTPUT"
fi

echo "Ollama is ready (pid ${pid}, models in ${models_dir})"
