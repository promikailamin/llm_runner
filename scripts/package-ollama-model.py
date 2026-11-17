#!/usr/bin/env python3
"""Package an Ollama model directory into release-friendly split tar parts."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path


def safe_name(model: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]", "-", model.replace(":", "-").replace("/", "-"))


def split_model_ref(model: str) -> tuple[str, str, str, str]:
    tag = "latest"
    name = model
    last = model.rsplit("/", 1)[-1]
    if ":" in last:
        last_name, tag = last.rsplit(":", 1)
        name = model[: -(len(tag) + 1)]

    parts = name.split("/")
    if len(parts) == 1:
        registry, namespace, repo = "registry.ollama.ai", "library", parts[0]
    elif len(parts) == 2:
        registry, namespace, repo = "registry.ollama.ai", parts[0], parts[1]
    else:
        registry, namespace, repo = parts[0], parts[1], "/".join(parts[2:])
    return registry, namespace, repo, tag


def find_manifest(models_dir: Path, model: str) -> Path:
    registry, namespace, repo, tag = split_model_ref(model)
    candidate = models_dir / "manifests" / registry / namespace / repo / tag
    if candidate.is_file():
        return candidate

    matches = sorted(models_dir.glob(f"manifests/**/{repo}/{tag}"))
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise FileNotFoundError(
            f"manifest for {model} not found under {models_dir / 'manifests'}"
        )
    raise RuntimeError(f"multiple manifests matched {model}: {matches}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024 * 8), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_manifest_and_blobs(
    manifest_path: Path, models_dir: Path, stage: Path, verify: bool
) -> list[str]:
    with manifest_path.open(encoding="utf-8") as handle:
        manifest = json.load(handle)

    rel_manifest = manifest_path.relative_to(models_dir)
    staged_manifest = stage / rel_manifest
    staged_manifest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(manifest_path, staged_manifest)

    digests: list[str] = []
    config = manifest.get("config")
    if isinstance(config, dict) and config.get("digest"):
        digests.append(config["digest"])
    for layer in manifest.get("layers", []):
        if isinstance(layer, dict) and layer.get("digest"):
            digests.append(layer["digest"])

    unique = list(dict.fromkeys(digests))
    for digest in unique:
        blob_name = digest.replace(":", "-")
        source = models_dir / "blobs" / blob_name
        if not source.is_file():
            raise FileNotFoundError(f"blob {blob_name} referenced by {manifest_path} is missing")
        target = stage / "blobs" / blob_name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        if verify and sha256_file(source) != digest.split(":", 1)[-1]:
            raise RuntimeError(f"digest mismatch for {source}")

    return unique


def archive_stage(stage: Path, archive: Path) -> None:
    with tarfile.open(archive, "w") as tar:
        for path in sorted(stage.rglob("*")):
            tar.add(path, arcname=str(path.relative_to(stage)))


def split_archive(archive: Path, output_dir: Path, chunk_size: str, name: str) -> list[Path]:
    prefix = output_dir / f"{name}.tar.part-"
    subprocess.run(
        ["split", "-b", chunk_size, "-d", "-a", "3", str(archive), str(prefix)],
        check=True,
    )
    parts = sorted(output_dir.glob(f"{name}.tar.part-*"))
    if not parts:
        raise RuntimeError(f"split produced no parts for {archive}")
    return parts


def update_catalog(catalog_path: Path, entry: dict) -> None:
    if catalog_path.is_file():
        with catalog_path.open(encoding="utf-8") as handle:
            catalog = json.load(handle)
    else:
        catalog = {"models": []}

    catalog.setdefault("models", [])
    catalog["models"] = [
        item for item in catalog["models"] if item.get("model") != entry["model"]
    ]
    catalog["models"].append(entry)
    catalog["models"].sort(key=lambda item: item["model"])
    catalog["generated_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()

    with catalog_path.open("w", encoding="utf-8") as handle:
        json.dump(catalog, handle, indent=2)
        handle.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--models-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--chunk-size", default="1900M")
    parser.add_argument("--catalog", type=Path)
    parser.add_argument("--checksums", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()

    model = args.model
    name = safe_name(model)
    models_dir = args.models_dir.expanduser().resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    manifest_path = find_manifest(models_dir, model)
    with manifest_path.open(encoding="utf-8") as handle:
        manifest = json.load(handle)

    stage = output_dir / f".stage-{name}"
    archive = output_dir / f"{name}.tar"
    if stage.exists():
        shutil.rmtree(stage)
    if archive.exists():
        archive.unlink()

    digests = copy_manifest_and_blobs(manifest_path, models_dir, stage, args.verify)
    archive_stage(stage, archive)
    shutil.rmtree(stage)
    parts = split_archive(archive, output_dir, args.chunk_size, name)
    archive.unlink()

    checksum_lines = []
    part_entries = []
    for part in parts:
        digest = sha256_file(part)
        checksum_lines.append(f"{digest}  {part.name}")
        part_entries.append({"name": part.name, "sha256": digest, "size": part.stat().st_size})

    if args.checksums:
        args.checksums.parent.mkdir(parents=True, exist_ok=True)
        with args.checksums.open("a", encoding="utf-8") as handle:
            for line in checksum_lines:
                handle.write(line + "\n")

    entry = {
        "model": model,
        "safe_name": name,
        "manifest": manifest_path.relative_to(models_dir).as_posix(),
        "manifest_digest": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
        "blobs": digests,
        "parts": part_entries,
        "total_size": sum(item["size"] for item in part_entries),
    }
    if args.catalog:
        update_catalog(args.catalog, entry)

    print(json.dumps(entry, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
