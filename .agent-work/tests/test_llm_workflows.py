import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"


def load_workflow(name):
    with (WORKFLOWS / name).open(encoding="utf-8") as handle:
        data = yaml.safe_load(handle)
    if True in data and "on" not in data:
        data["on"] = data.pop(True)
    return data


class WorkflowTests(unittest.TestCase):
    def test_llm_workflow(self):
        workflow = load_workflow("llm.yml")
        dispatch = workflow["on"]["workflow_dispatch"]["inputs"]
        self.assertEqual(dispatch["timeout_minutes"]["type"], "number")
        options = dispatch["model"]["options"]
        for model in (
            "starcoder2:7b",
            "deepseek-coder:6.7b",
            "qwen2.5-coder:7b",
            "qwen2.5-coder:3b",
        ):
            self.assertIn(model, options)
        self.assertIn("serve", workflow["jobs"])
        self.assertIn("client", workflow["jobs"])
        serve = workflow["jobs"]["serve"]
        client = workflow["jobs"]["client"]
        self.assertEqual(serve["timeout-minutes"], 360)
        self.assertEqual(serve["outputs"]["address"], "${{ steps.tunnel.outputs.address }}")
        self.assertNotIn("needs", client)
        steps = serve["steps"]
        commands = "\n".join(step.get("run", "") for step in steps)
        self.assertIn("scripts/install-ollama.sh", commands)
        self.assertIn("scripts/start-tunnel.sh", commands)
        self.assertIn("scripts/install-bore.sh", commands)
        upload = [step for step in steps if step.get("uses", "").startswith("actions/upload-artifact")]
        self.assertEqual(len(upload), 1)
        self.assertEqual(upload[0]["with"]["name"], "llm-tunnel-${{ github.run_id }}")
        client_commands = "\n".join(step.get("run", "") for step in client["steps"])
        self.assertIn("scripts/wait-for-tunnel.sh", client_commands)
        self.assertIn("max_wait_seconds=$(( (TIMEOUT_MINUTES + 30) * 60 ))", client_commands)
        self.assertIn("scripts/probe-tunnel.sh", client_commands)

    def test_models_release_workflow(self):
        workflow = load_workflow("models-release.yml")
        dispatch = workflow["on"]["workflow_dispatch"]["inputs"]
        for name in (
            "include_starcoder2",
            "include_deepseek_coder",
            "include_qwen25_coder_7b",
            "include_qwen25_coder_3b",
        ):
            self.assertEqual(dispatch[name]["type"], "boolean")
        self.assertEqual(dispatch["release_tag"]["default"], "llm-models")
        job = workflow["jobs"]["publish"]
        commands = "\n".join(step.get("run", "") for step in job["steps"])
        for expected in (
            "scripts/install-ollama.sh",
            "scripts/package-ollama-model.py",
            "gh release create",
            "gh release upload",
        ):
            self.assertIn(expected, commands)

    def test_ollama_binds_all_interfaces_for_tunnel_host_header(self):
        install_script = (ROOT / "scripts" / "install-ollama.sh").read_text(encoding="utf-8")
        self.assertIn("OLLAMA_HOST=0.0.0.0:11434", install_script)

    def test_scripts_exist_and_are_parseable(self):
        scripts = sorted((ROOT / "scripts").glob("*.sh"))
        self.assertGreaterEqual(len(scripts), 8)
        for script in scripts:
            result = subprocess.run(
                ["bash", "-n", str(script)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, f"{script}: {result.stderr}")

    def test_resolve_model(self):
        env = os.environ.copy()
        env["GITHUB_ENV"] = "/dev/null"
        result = subprocess.run(
            ["bash", "scripts/resolve-model.sh", "qwen2.5-coder:3b"],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "qwen2.5-coder:3b")

        invalid = subprocess.run(
            ["bash", "scripts/resolve-model.sh", "bad model"],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(invalid.returncode, 0)

    def test_start_tunnel_writes_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            fake_bore = tmp_path / "bore"
            fake_bore.write_text(
                "#!/usr/bin/env bash\n"
                "[ \"${1:-}\" = \"local\" ] || exit 2\n"
                "echo 'INFO listening at bore.pub:45678'\n"
                "trap 'exit 0' TERM INT\n"
                "while true; do sleep 1; done\n",
                encoding="utf-8",
            )
            fake_bore.chmod(0o755)
            output = tmp_path / "tunnel.json"
            env = os.environ.copy()
            env["BORE_BIN"] = str(fake_bore)
            result = subprocess.run(
                [
                    "bash",
                    "scripts/start-tunnel.sh",
                    "11434",
                    str(output),
                    "test-model:1b",
                    "1",
                    "1",
                ],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(data["address"], "bore.pub:45678")
            self.assertEqual(data["port"], 45678)
            self.assertEqual(data["model"], "test-model:1b")
            pid = data["pid"]
            try:
                os.kill(pid, 15)
            except ProcessLookupError:
                pass

    def test_package_ollama_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            models = tmp_path / "models"
            manifest = models / "manifests" / "registry.ollama.ai" / "library" / "testmodel" / "1b"
            manifest.parent.mkdir(parents=True)
            config = "a" * 64
            layer = "b" * 64
            (models / "blobs").mkdir(parents=True)
            (models / "blobs" / f"sha256-{config}").write_bytes(b'{"model":"test"}')
            (models / "blobs" / f"sha256-{layer}").write_bytes(b"weights" * 100)
            manifest.write_text(
                json.dumps(
                    {
                        "schemaVersion": 2,
                        "config": {"digest": f"sha256:{config}", "size": 16},
                        "layers": [{"digest": f"sha256:{layer}", "size": 700}],
                    }
                ),
                encoding="utf-8",
            )
            stage = tmp_path / "stage"
            catalog = stage / "models-manifest.json"
            checksums = stage / "SHA256SUMS"
            result = subprocess.run(
                [
                    "python3",
                    "scripts/package-ollama-model.py",
                    "--model",
                    "testmodel:1b",
                    "--models-dir",
                    str(models),
                    "--output-dir",
                    str(stage),
                    "--chunk-size",
                    "1M",
                    "--catalog",
                    str(catalog),
                    "--checksums",
                    str(checksums),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            parts = sorted(stage.glob("testmodel-1b.tar.part-*"))
            self.assertEqual(len(parts), 1)
            extract = tmp_path / "extract"
            extract.mkdir()
            with parts[0].open("rb") as handle:
                subprocess.run(["tar", "-xf", "-", "-C", str(extract)], stdin=handle, check=True)
            self.assertTrue((extract / "manifests" / "registry.ollama.ai" / "library" / "testmodel" / "1b").is_file())
            self.assertTrue((extract / "blobs" / f"sha256-{layer}").is_file())
            data = json.loads(catalog.read_text(encoding="utf-8"))
            self.assertEqual(data["models"][0]["model"], "testmodel:1b")
            self.assertIn("testmodel-1b.tar.part-000", checksums.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
