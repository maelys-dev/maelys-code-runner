#!/usr/bin/env python3
"""Build a deterministic maelys-code-runner release archive."""

from __future__ import annotations

import gzip
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile


ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()


def run(*arguments: str, env: dict[str, str] | None = None) -> None:
    subprocess.run(arguments, cwd=ROOT, env=env, check=True)


def source_date_epoch() -> int:
    configured = os.environ.get("SOURCE_DATE_EPOCH")
    if configured:
        return int(configured)
    try:
        value = subprocess.check_output(
            ["git", "log", "-1", "--format=%ct"],
            cwd=ROOT,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        return int(value)
    except (subprocess.CalledProcessError, ValueError):
        return 0


def add_tree(archive: tarfile.TarFile, source: Path, archive_root: str, epoch: int) -> None:
    entries = [source, *sorted(source.rglob("*"), key=lambda path: path.as_posix())]
    for path in entries:
        relative = path.relative_to(source)
        name = archive_root if relative == Path(".") else f"{archive_root}/{relative.as_posix()}"
        info = archive.gettarinfo(str(path), arcname=name)
        info.uid = 0
        info.gid = 0
        info.uname = "root"
        info.gname = "root"
        info.mtime = epoch
        if path.is_file():
            with path.open("rb") as stream:
                archive.addfile(info, stream)
        else:
            archive.addfile(info)


def main() -> int:
    if len(sys.argv) != 2 or not sys.argv[1] or "/" in sys.argv[1]:
        print("usage: package-release.py TARGET", file=sys.stderr)
        return 2
    target = sys.argv[1]
    package_name = f"maelys-code-runner-{VERSION}-{target}"
    build = ROOT / "build" / f"package-{target}"
    stage = build / "stage"
    dist = ROOT / "dist"
    shutil.rmtree(build, ignore_errors=True)
    stage.mkdir(parents=True)
    dist.mkdir(exist_ok=True)

    run("cmake", "-S", ".", "-B", str(build), "-DCMAKE_BUILD_TYPE=Release")
    run("cmake", "--build", str(build), "--parallel")
    environment = os.environ.copy()
    environment["DESTDIR"] = str(stage)
    run("cmake", "--install", str(build), "--prefix", "/usr/local", env=environment)

    prefix = stage / "usr" / "local"
    binary = prefix / "bin" / "maelys-code-runner"
    observed = subprocess.check_output([str(binary), "--version"], text=True).strip()
    if observed != VERSION:
        raise RuntimeError(f"installed binary reports {observed}, expected {VERSION}")
    for license_name in ("LICENSE.quickjs-ng", "LICENSE.jansson"):
        license_path = (
            prefix / "share" / "doc" / "maelys-code-runner" / "licenses" / license_name
        )
        if not license_path.is_file():
            raise RuntimeError(f"installed package is missing {license_name}")

    uncompressed = build / f"{package_name}.tar"
    with tarfile.open(uncompressed, "w", format=tarfile.PAX_FORMAT) as archive:
        add_tree(archive, prefix, package_name, source_date_epoch())
    output = dist / f"{package_name}.tar.gz"
    with uncompressed.open("rb") as source, output.open("wb") as destination:
        with gzip.GzipFile(filename="", mode="wb", fileobj=destination, mtime=0) as compressed:
            shutil.copyfileobj(source, compressed)

    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    sidecar = output.with_name(output.name + ".sha256")
    sidecar.write_text(f"{digest}  {output.name}\n", encoding="ascii")
    print(output)
    print(sidecar)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
