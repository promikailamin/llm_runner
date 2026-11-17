# llm_runner

Chess screenshot analysis project.

This repository also provides GitHub Actions for serving coding LLMs through
Ollama and exposing the Ollama API through a public [bore.pub](https://bore.pub)
tunnel.

## Models

The model release workflow supports these Ollama models:

| Model | Ollama reference |
| --- | --- |
| StarCoder2 7B | `starcoder2:7b` |
| DeepSeek-Coder 6.7B | `deepseek-coder:6.7b` |
| Qwen2.5-Coder 7B | `qwen2.5-coder:7b` |
| Qwen2.5-Coder 3B | `qwen2.5-coder:3b` |

The [`llm-models`](https://github.com/promikailamin/llm_runner/releases/tag/llm-models)
release contains packaged assets for all four models. Assets are split below
GitHub's 2 GiB per-file limit and `SHA256SUMS` covers every part. Re-run
**Publish LLM models** to regenerate or update the release.

## Serve a model with a public tunnel

Run **Actions > LLM over bore.pub > Run workflow**.

Inputs:

- `model`: one of the four models above, or `custom`.
- `custom_model`: any Ollama model reference when `model=custom`.
- `model_source`:
  - `ollama`: pull the model directly from the Ollama registry.
  - `release`: restore the packaged model from the model release.
- `release_tag`: release that contains packaged model assets (`llm-models` by default).
- `timeout_minutes`: how long to keep Ollama and the tunnel alive (1-300).
- `warmup`: run one small inference request before publishing the tunnel.
- `run_agent`: build and run the C++ CLI agent against the tunnel.
- `agent_task`: task passed to the CLI agent.
- `agent_max_steps`: maximum number of agent steps.
- `agent_timeout_minutes`: maximum minutes for the CLI agent.
- `agent_api`: `auto`, `ollama`, or `openai`.

Only one **LLM over bore.pub** run executes at a time. When another run is
dispatched while a run is active, the new run stays pending instead of starting
a second tunnel.

The workflow has three jobs:

1. `serve` installs Ollama, pulls or restores the selected model, starts
   `ollama serve` on `127.0.0.1:11434`, starts `bore local 11434 --to bore.pub`,
   uploads `tunnel.json` as the `llm-tunnel-<run_id>` artifact, and publishes or
   recreates the `tunnel` release.
2. `agent` runs at the same time. It builds the C++ CLI agent, waits for the
   tunnel artifact, and runs the agent against `http://<host:port>` with the
   requested task.
3. `cleanup` deletes the `tunnel` release when the serving window ends.

The jobs run concurrently because a bore tunnel is owned by the `bore` process.
If the agent job waited for `serve` with `needs`, the tunnel would already be
closed. The artifact is the hand-off between jobs; the `serve` job also
publishes `address`, `host`, `port`, and `model` as job outputs.

The `tunnel` release is recreated for every run and contains:

- the tunnel address (`host:port`),
- the API base URL,
- the model,
- the action (run) ID and attempt,
- the action URL,
- the run start time, timeout, and expiry time,
- `tunnel.json` as an asset.

The release is deleted when the keep-alive finishes, when the run times out, or
when the run is cancelled by a user or the workflow. The keep-alive step also
installs a signal trap so cancellation cleans up the release before the runner
disappears.

The connection address is printed in the `serve` job summary and log, for example:

```
Tunnel ready at bore.pub:32271
```

Use it as an OpenAI-compatible/Ollama API base URL:

```bash
curl http://bore.pub:32271/api/tags
curl http://bore.pub:32271/api/generate \
  -d '{"model":"qwen2.5-coder:3b","prompt":"Say hello","stream":false}'
```

### CLI agent

`cli/` contains a C++17 coding agent that connects to the tunnel, keeps a
conversation with the model, and gives the model shell and file tools:

- `run_shell`: run bash commands and return exit code, stdout, and stderr.
- `read_file`, `write_file`, and `list_dir`: inspect and edit the workspace.

It supports native Ollama/OpenAI tool calling and a text-based fallback for
models that do not support native tools. See [`cli/README.md`](cli/README.md) for
build and usage details.

```bash
llm-agent \
  --tunnel tunnel.json \
  --task "Inspect the repository, run the tests, and summarize the result" \
  --max-steps 30 \
  --timeout 1800 \
  --log agent.log
```

The **Build CLI agent** workflow in `.github/workflows/build.yml` builds and
tests static `linux-x86_64` and `linux-aarch64` binaries. The aarch64 binary is
cross-compiled and tested under `qemu-aarch64`; both binaries are uploaded as
workflow artifacts.

### Security

`bore.pub` creates a public tunnel and Ollama has no authentication by default.
Anyone who learns the address can use the model until the job's timeout ends.
Do not send secrets or private data through the tunnel. The CLI agent can run
shell commands, so run it only in a disposable environment when the model is
untrusted.

### Runner limits

GitHub-hosted runners have no GPU. 3B models are practical on CPU; 7B models are
much slower. The 7B models may also need most of the runner's available memory.

## Publish models to a GitHub release

Run **Actions > Publish LLM models > Run workflow**.

The workflow:

1. Installs and starts Ollama.
2. Pulls the selected models.
3. Exports each model's manifest and blobs.
4. Creates `<model>.tar.part-*` files below 2000 MB (GitHub's release asset limit
   is 2 GiB per file).
5. Writes `SHA256SUMS` and `models-manifest.json`.
6. Creates or updates the release (default tag: `llm-models`) and uploads the assets.

After the release is populated, dispatch **LLM over bore.pub** with
`model_source=release` to avoid pulling the model again.

To restore a model manually:

```bash
cat qwen2.5-coder-3b.tar.part-* | tar -xf - -C ~/.ollama/models
```

## Repository layout

- `.github/workflows/llm.yml`: serve a model, publish the `tunnel` release, and run the CLI agent.
- `.github/workflows/models-release.yml`: package models into a release.
- `.github/workflows/build.yml`: build and test the CLI agent for x86_64 and aarch64.
- `.github/workflows/tunnel-cleanup.yml`: delete a stale `tunnel` release when no run is active.
- `cli/`: C++17 CLI coding agent with shell and file tools.
- `scripts/install-ollama.sh`: install and start Ollama.
- `scripts/install-bore.sh`: download and verify the pinned bore release.
- `scripts/start-tunnel.sh`: start bore and write `tunnel.json`.
- `scripts/publish-tunnel-release.sh`: create or recreate the `tunnel` release.
- `scripts/delete-tunnel-release.sh`: delete the `tunnel` release.
- `scripts/cleanup-stale-tunnel-release.sh`: safety-net cleanup for cancelled runs.
- `scripts/wait-for-tunnel.sh`: retrieve the tunnel artifact in the agent job.
- `scripts/probe-tunnel.sh`: health-probe the public tunnel.
- `scripts/keep-alive.sh`: keep the server alive for the selected timeout.
- `scripts/package-ollama-model.py`: export and split an Ollama model for release.
- `scripts/restore-release-model.sh`: restore a split model from a release.
