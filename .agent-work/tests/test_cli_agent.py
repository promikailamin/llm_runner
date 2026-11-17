import json
import os
import shutil
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLI = ROOT / "cli"
BINARY = None
BUILD_DIR = None


class JsonHandler(BaseHTTPRequestHandler):
    calls = 0
    mode = "ollama-native"

    def log_message(self, fmt, *args):
        pass

    def send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/api/tags", "/v1/models"):
            self.send_json(200, {"models": [], "data": []})
        else:
            self.send_json(404, {"error": "not found"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length) or b"{}")
        type(self).calls += 1
        if type(self).mode == "openai":
            self.handle_openai(request)
        elif type(self).mode == "ollama-text":
            self.handle_ollama_text(request)
        else:
            self.handle_ollama_native(request)

    def handle_ollama_native(self, request):
        if type(self).calls == 1:
            message = {
                "role": "assistant",
                "content": "",
                "tool_calls": [
                    {
                        "function": {
                            "name": "run_shell",
                            "arguments": {"command": "printf cli-native-ok"},
                        }
                    }
                ],
            }
        else:
            tools = [m for m in request.get("messages", []) if m.get("role") == "tool"]
            assert tools and "cli-native-ok" in tools[-1].get("content", ""), tools
            message = {"role": "assistant", "content": "FINAL: native flow complete"}
        self.send_json(200, {"model": "mock", "message": message, "done": True})

    def handle_ollama_text(self, request):
        if type(self).calls == 1:
            message = {
                "role": "assistant",
                "content": '```tool\n{"name":"run_shell","arguments":{"command":"printf cli-text-ok"}}\n```',
            }
        else:
            users = [m for m in request.get("messages", []) if m.get("role") == "user"]
            assert users and "cli-text-ok" in users[-1].get("content", ""), users[-1]
            message = {"role": "assistant", "content": "FINAL: text flow complete"}
        self.send_json(200, {"model": "mock", "message": message, "done": True})

    def handle_openai(self, request):
        if type(self).calls == 1:
            message = {
                "role": "assistant",
                "content": "",
                "tool_calls": [
                    {
                        "id": "call_1",
                        "type": "function",
                        "function": {
                            "name": "run_shell",
                            "arguments": json.dumps({"command": "printf cli-openai-ok"}),
                        },
                    }
                ],
            }
        else:
            tools = [m for m in request.get("messages", []) if m.get("role") == "tool"]
            assert tools and "cli-openai-ok" in tools[-1].get("content", ""), tools
            message = {"role": "assistant", "content": "FINAL: openai flow complete"}
        self.send_json(200, {"choices": [{"message": message}]})


def build_cli():
    global BINARY, BUILD_DIR
    if BINARY is not None:
        return BINARY
    BUILD_DIR = tempfile.mkdtemp(prefix="llm-agent-build-")
    configure = subprocess.run(
        [
            "cmake",
            "-S",
            str(CLI),
            "-B",
            BUILD_DIR,
            "-DCMAKE_BUILD_TYPE=Release",
        ],
        capture_output=True,
        text=True,
    )
    if configure.returncode != 0:
        raise RuntimeError(configure.stderr)
    build = subprocess.run(
        ["cmake", "--build", BUILD_DIR, "--parallel", "2"],
        capture_output=True,
        text=True,
    )
    if build.returncode != 0:
        raise RuntimeError(build.stdout + build.stderr)
    BINARY = Path(BUILD_DIR) / "llm-agent"
    return BINARY


@unittest.skipUnless(shutil.which("cmake") and shutil.which("g++"), "cmake or g++ missing")
class CliAgentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = build_cli()

    @classmethod
    def tearDownClass(cls):
        if BUILD_DIR:
            shutil.rmtree(BUILD_DIR, ignore_errors=True)

    def start_server(self, mode):
        handler = type("Handler", (JsonHandler,), {"calls": 0, "mode": mode})
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.shutdown)
        self.addCleanup(server.server_close)
        return server, handler

    def run_agent(self, *args):
        env = os.environ.copy()
        env.pop("LLM_TUNNEL_URL", None)
        env.pop("TUNNEL_URL", None)
        env.pop("LLM_MODEL", None)
        return subprocess.run(
            [str(self.binary), "--quiet", *args],
            capture_output=True,
            text=True,
            timeout=120,
            env=env,
        )

    def test_self_test(self):
        result = subprocess.run(
            [str(self.binary), "--self-test"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SELF-TEST PASSED", result.stdout)

    def test_native_ollama_tool_flow_with_tunnel_file(self):
        server, _ = self.start_server("ollama-native")
        base = f"http://127.0.0.1:{server.server_address[1]}"
        with tempfile.TemporaryDirectory() as tmp:
            tunnel = Path(tmp) / "tunnel.json"
            tunnel.write_text(
                json.dumps({"url": base, "address": "127.0.0.1:1", "model": "mock"}),
                encoding="utf-8",
            )
            result = self.run_agent(
                "--tunnel",
                str(tunnel),
                "--task",
                "run the native test",
                "--max-steps",
                "5",
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("native flow complete", result.stdout)

    def test_text_tool_fallback(self):
        server, _ = self.start_server("ollama-text")
        base = f"http://127.0.0.1:{server.server_address[1]}"
        result = self.run_agent(
            "--url",
            base,
            "--model",
            "mock",
            "--task",
            "run the text test",
            "--max-steps",
            "5",
            "--no-native-tools",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("text flow complete", result.stdout)

    def test_openai_tool_flow(self):
        server, _ = self.start_server("openai")
        base = f"http://127.0.0.1:{server.server_address[1]}"
        result = self.run_agent(
            "--url",
            base,
            "--api",
            "openai",
            "--model",
            "mock",
            "--task",
            "run the openai test",
            "--max-steps",
            "5",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("openai flow complete", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
