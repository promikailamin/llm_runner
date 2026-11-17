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
- `run_client`: run the companion client job that probes the tunnel.
- `probe_interval_seconds`: delay between client health probes.

The workflow has two jobs that run at the same time:

1. `serve` installs Ollama, pulls/restores the selected model, starts `ollama serve`
   on `127.0.0.1:11434`, starts `bore local 11434 --to bore.pub`, and uploads
   `tunnel.json` as the `llm-tunnel-<run_id>` artifact.
2. `client` waits for that artifact, reads the `host:port`, and probes
   `http://<host:port>/api/tags` until the selected timeout expires.

The jobs run concurrently because a bore tunnel is owned by the `bore` process.
If the client waited for the `serve` job to finish with `needs`, the tunnel would
already be closed. The artifact is the hand-off between jobs; the `serve` job also
publishes `address`, `host`, `port`, and `model` as job outputs.

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

### Security

`bore.pub` creates a public tunnel and Ollama has no authentication by default.
Anyone who learns the address can use the model until the job's timeout ends.
Do not send secrets or private data through the tunnel.

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

- `.github/workflows/llm.yml`: serve a model and publish its bore.pub address.
- `.github/workflows/models-release.yml`: package models into a release.
- `scripts/install-ollama.sh`: install and start Ollama.
- `scripts/install-bore.sh`: download and verify the pinned bore release.
- `scripts/start-tunnel.sh`: start bore and write `tunnel.json`.
- `scripts/wait-for-tunnel.sh`: retrieve the tunnel artifact in the client job.
- `scripts/probe-tunnel.sh`: health-probe the public tunnel.
- `scripts/keep-alive.sh`: keep the server alive for the selected timeout.
- `scripts/package-ollama-model.py`: export and split an Ollama model for release.
- `scripts/restore-release-model.sh`: restore a split model from a release.
