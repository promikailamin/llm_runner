# llm-agent

`llm-agent` is a small C++17 coding agent for the Ollama servers exposed by the
**LLM over bore.pub** workflow. It connects to a tunnel address, keeps a chat
conversation with the model, and lets the model inspect the workspace and run
shell commands through tool calls.

The binary is self-contained: it implements HTTP/1.1 over POSIX sockets and
vendors `nlohmann/json`. It has no runtime dependency on libcurl or OpenSSL.

## Build

```bash
cmake -S cli -B build/cli -DCMAKE_BUILD_TYPE=Release -DLLM_AGENT_STATIC=ON
cmake --build build/cli --parallel
./build/cli/llm-agent --self-test
```

The `Build CLI agent` workflow builds `linux-x86_64` and `linux-aarch64`
binaries. The aarch64 build is cross-compiled with
`aarch64-linux-gnu-g++` and tested under `qemu-aarch64`.

## Usage

Use the `tunnel.json` artifact or the `tunnel` release:

```bash
llm-agent \
  --tunnel tunnel.json \
  --task "Inspect the repository, run the tests, and summarize the result" \
  --max-steps 30 \
  --timeout 1800 \
  --log agent.log
```

Or use an address directly:

```bash
llm-agent \
  --url http://bore.pub:32271 \
  --model qwen2.5-coder:3b \
  --task "Fix the failing test"
```

Options:

| Option | Description |
| --- | --- |
| `--tunnel PATH` | Read `url`, `address`, and `model` from a tunnel JSON file. |
| `--url URL` | API base URL. Overrides the tunnel URL. |
| `--model NAME` | Ollama model. Overrides the tunnel model. |
| `--api NAME` | `ollama`, `openai`, or `auto`. Default: `ollama`. |
| `--task TEXT` | Task for the agent. Positional text also works. |
| `--task-file PATH` | Read the task from a file. |
| `--system TEXT` | Override the built-in system prompt. |
| `--system-prompt PATH` | Read a system prompt from a file. |
| `--cwd PATH` | Working directory. Default: current directory. |
| `--max-steps N` | Maximum agent iterations. Default: 40. |
| `--timeout SECONDS` | Overall agent time limit. Default: 1800. |
| `--shell-timeout SEC` | Default shell command timeout. Default: 120. |
| `--max-output N` | Maximum characters per tool result. Default: 20000. |
| `--temperature VALUE` | Sampling temperature. Default: 0.2. |
| `--log PATH` | Append a detailed session log. |
| `--no-native-tools` | Always use the text tool-call protocol. |
| `--quiet` | Suppress progress output on stderr. |
| `--self-test` | Run local checks without network access. |

Environment variables `LLM_TUNNEL_URL`, `TUNNEL_URL`, and `LLM_MODEL` are used
when the corresponding options are not provided.

## Tools

The model can call:

- `run_shell` - run a bash command, returning exit code, stdout, and stderr.
- `read_file` - read a text file with optional line offset and limit.
- `write_file` - create or overwrite a text file.
- `list_dir` - list files and directories.

Native tool calls are used when the server and model support them. For models
without native tool calling, the agent falls back to a text protocol: the model
replies with one fenced block such as

````text
```tool
{"name":"run_shell","arguments":{"command":"ls -la"}}
```
````

The agent executes the call, appends the result to the conversation, and
continues until the model returns a final answer or a limit is reached.

## Security

The agent executes shell commands on the machine where it runs. The bore.pub
tunnel is public and Ollama does not authenticate requests. Do not send secrets
through the tunnel, and run the agent only in a disposable environment when the
model is untrusted.
