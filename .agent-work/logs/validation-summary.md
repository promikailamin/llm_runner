# Validation summary

Date: 2026-11-17

## Static checks

- `python3 -m unittest discover -s .agent-work/tests -p 'test_*.py' -v`: 7 tests passed.
- `yamllint` on both workflows: passed.
- `shellcheck -x scripts/*.sh`: passed.
- `actionlint` 1.7.12 on both workflows: passed.

## End-to-end checks

- `scripts/install-ollama.sh`: installed Ollama 0.40.16 and started it successfully.
  Log: `install-ollama-test.log`.
- `scripts/pull-model.sh qwen2.5-coder:0.5b`: pulled a real model successfully.
  Log: `pull-model-test.log`.
- `scripts/start-tunnel.sh`: created a real `bore.pub` tunnel and wrote `tunnel.json`.
  Log: `start-tunnel-test.log`.
- Public Ollama API through the tunnel: `/api/tags` returned the model and
  `/api/generate` returned `OK.`.
- `scripts/probe-tunnel.sh`: 6/6 probes succeeded over 1 minute.
  Log: `probe-tunnel-test.log`.
- `scripts/package-ollama-model.py`: packaged the real 0.5B model into a split
  release archive, verified SHA256SUMS, and extracted it successfully.
  Log: `package-model-test.json`.

## Important fix found during testing

Ollama 0.40 rejects requests whose Host header is not loopback when the server is
bound to `127.0.0.1`. Requests forwarded by bore use a `bore.pub` Host header and
received HTTP 403. `scripts/install-ollama.sh` therefore binds the server to
`0.0.0.0:11434`, which disables that loopback-only host check and allows the
tunnel to work. Local health checks still use `127.0.0.1:11434`.
