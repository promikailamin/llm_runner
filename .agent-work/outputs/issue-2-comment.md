Implemented in `main` through `171bb98`.

## Workflow

- `.github/workflows/llm.yml` now just serves the model behind a bore.pub tunnel and runs the CLI agent against it.
- It creates or recreates the `tunnel` release with `tunnel.json`, the address, API URL, model, run start time, timeout, expiry, action ID, attempt, and action URL.
- The release is deleted from the serve-step trap, an `if: always()` step, a dedicated cleanup job, and a scheduled stale-release cleanup.
- Runs are serialized with `concurrency.group: llm-tunnel-${{ github.repository }}` and `cancel-in-progress: false`, so a second run stays pending.
- Deletion also removes a leftover `tunnel` git tag.

## CLI

- Added a C++17 CLI under `cli/` with `llm-agent`.
- Native Ollama/OpenAI tool calls plus text tool-call fallback.
- Tools: `run_shell`, `read_file`, `write_file`, `list_dir`.
- Shell command output is returned to the model; timeouts and step/loop limits are enforced.
- Repeated identical tool calls are capped, then the agent asks for a final answer or exits with a clear error.
- Unknown text tool names are treated as final answers instead of being executed.
- `llm.yml` builds, self-tests, waits for `tunnel.json`, and runs the agent through the tunnel.

## Builds

- Added `.github/workflows/build.yml` with static x86_64 and aarch64 builds.
- Both targets run `llm-agent --self-test`; the arm64 binary is tested under `qemu-aarch64`.
- Packaged binaries are uploaded as artifacts.

## Validation

- Build run `44118384617`: x86_64 and aarch64 passed.
- `LLM over bore.pub` run `44119911428` on `171bb98`: all jobs passed. The agent connected through bore.pub, finished in one step, and returned the requested marker.
- After the run the `tunnel` release and tag were both absent.
- Two back-to-back dispatches showed one run `in_progress` and the second `pending`.
- Cancelling a serving run deleted the release; cancelling the queued run also cleaned up.
- Python tests: 16 passing. Shell scripts pass `shellcheck`; workflows pass `actionlint`.

The end-to-end run used `qwen2.5-coder:0.5b` via the `custom_model` input for speed.
