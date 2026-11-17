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

## GitHub Actions end-to-end runs

- `llm.yml` with `qwen2.5-coder:0.5b`, timeout 1 minute:
  run 44094796185 completed. The client read the tunnel artifact and made
  4/4 successful probes. Log: `llm-workflow-run-44094796185.log`.
- `models-release.yml` with `qwen2.5-coder:3b`:
  run 44095974252 completed and created release `llm-models`.
  Log: `models-release-3b.log`, release: `release-3b.json`.
- `models-release.yml` with all four requested models:
  run 44096468234 completed in 9m33s. Release assets total 13.49 GiB:
  - starcoder2-7b: 3 parts
  - deepseek-coder-6.7b: 2 parts
  - qwen2.5-coder-7b: 3 parts
  - qwen2.5-coder-3b: 1 part
  Log: `models-release-all.log`, release: `release-all.json`.
- `llm.yml` with `model_source=release`, `qwen2.5-coder:3b`, timeout 1 minute:
  run 44098147042 completed. The model was restored from release assets, the
  tunnel was created, and the client made 4/4 successful probes.
  Log: `llm-release-source-run-44098147042.log`.

# Validation summary for issue 2

Date: 2026-11-17

## Static checks

- `python3 -m unittest discover -s .agent-work/tests -p 'test_*.py' -v`: 14 tests passed.
- `actionlint` 1.7.12 on all workflows: passed.
- `shellcheck -x scripts/*.sh`: passed.
- `yamllint` on the new/modified workflows: passed (only the repository's existing
  `document-start` and `on` truthy style warnings).
- `cmake` build for x86_64 with `LLM_AGENT_STATIC=ON`: passed.
- `aarch64-linux-gnu-g++` cross build with `LLM_AGENT_STATIC=ON`: passed.
- `qemu-aarch64 build/cli-arm64/llm-agent --self-test`: passed.
- `ctest` in `build/cli`: 1/1 passed.

## CLI agent tests

- Local self-test covers URL parsing, tool schemas, fenced/tagged/unfenced tool
  call parsing, shell execution, shell timeout, and file tools.
- Mock Ollama `/api/chat` native tool call flow: passed.
- Mock Ollama text fallback tool flow: passed.
- Mock OpenAI `/v1/chat/completions` native tool call flow: passed.
- `--tunnel tunnel.json` URL/model loading: passed.

## Real Ollama validation

- Installed Ollama with `scripts/install-ollama.sh` and pulled
  `qwen2.5-coder:0.5b`.
- Ran `llm-agent` against `http://127.0.0.1:11434` with a task that asks it to
  execute `printf real-ollama-ok`.
- The model emitted a text tool call; the agent parsed it, executed the shell
  command, returned `real-ollama-ok`, and finished successfully.
- Log: `.agent-work/logs/cli-real-ollama.log`.

## Tunnel release scripts

- `publish-tunnel-release.sh` with a fake `gh` deleted/recreated the release,
  uploaded `tunnel.json`, and generated notes containing address, action ID,
  run attempt, action URL, run start, timeout, and expiry.
- `delete-tunnel-release.sh` and `cleanup-stale-tunnel-release.sh` were tested
  with a fake `gh` for both active-run and idle cases.
