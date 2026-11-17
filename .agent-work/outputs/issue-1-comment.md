Implemented the requested LLM model release and serving workflow.

## What was added

- `.github/workflows/models-release.yml`
  - `workflow_dispatch` with toggles for `starcoder2:7b`, `deepseek-coder:6.7b`, `qwen2.5-coder:7b`, `qwen2.5-coder:3b`, plus extra custom models.
  - Installs Ollama, pulls each selected model, exports its manifest/blobs, and packages it into release assets below GitHub's 2 GiB per-file limit.
  - Uploads assets incrementally per model to keep runner disk usage low.
  - Creates/updates the `llm-models` release and publishes `SHA256SUMS` plus `models-manifest.json`.
- `.github/workflows/llm.yml`
  - `workflow_dispatch` model selection (the four models or a custom Ollama reference), `model_source=ollama|release`, release tag, timeout (1-300 minutes), warmup toggle, and client probe settings.
  - Installs/starts Ollama, pulls or restores the selected model, starts `bore local 11434 --to bore.pub`, and exposes the address as `address`, `host`, `port`, and `model` outputs.
  - Uploads `tunnel.json` as `llm-tunnel-<run_id>`. The companion `client` job runs concurrently, retrieves the artifact, and probes the public endpoint for the selected timeout. The jobs cannot use `needs` because the tunnel dies when the server job ends.
- Helper scripts under `scripts/` for Ollama setup, model pull, bore install/tunnel, artifact handoff, probing, keep-alive, packaging, and release restore.
- `README.md` documentation and tests under `.agent-work/tests/`.

## Release

Created the public `llm-models` release with all four requested models:

- `starcoder2-7b` - 3 parts
- `deepseek-coder-6.7b` - 2 parts
- `qwen2.5-coder-7b` - 3 parts
- `qwen2.5-coder-3b` - 1 part

Total release assets: 11 files, 14,483,738,447 bytes (~13.49 GiB), plus `SHA256SUMS` and `models-manifest.json`.

https://github.com/promikailamin/llm_runner/releases/tag/llm-models

## Validation

- 7 unit tests pass.
- `actionlint`, `yamllint` on the new workflows, and `shellcheck` on all scripts pass.
- Real `llm.yml` run with `qwen2.5-coder:0.5b`, 1-minute timeout: run 44094796185 passed; client retrieved the tunnel artifact and completed 4/4 probes.
- Real `models-release.yml` run with all four models: run 44096468234 passed in 9m33s and populated the release.
- Real `llm.yml` run with `model_source=release`, `qwen2.5-coder:3b`, 1-minute timeout: run 44098147042 passed; the model was restored from release assets, the tunnel was created, and the client completed 4/4 probes.
- During testing, Ollama 0.40 rejected bore-forwarded requests with HTTP 403 because of its loopback Host-header check. `scripts/install-ollama.sh` now binds Ollama to `0.0.0.0:11434` so the public tunnel works; local health checks still use `127.0.0.1:11434`.

## Commits

Latest commit: `708d8a7` on `main`. The implementation commits also include `f0fb582`, `4d5cf98`, `b372395`, `b5b7680`.

## Notes

- GitHub-hosted runners have no GPU, so 3B models are practical and 7B inference is slow.
- The bore.pub tunnel is public and Ollama has no authentication. Anyone with the address can use the model until the timeout ends.
- The model release is large (~13.5 GiB); rerunning **Publish LLM models** regenerates or updates the assets.
