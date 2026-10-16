#!/usr/bin/env python3
"""Build release archives from one clean source tree and binary tree."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

try:
    from .fork_version import parse_version
except ImportError:  # Running the file directly from scripts/.
    from fork_version import parse_version


class PackageError(RuntimeError):
    pass


def _git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args], check=True, capture_output=True, text=True,
    )
    return result.stdout.strip()


def _zip_stage(stage: Path, destination: Path, include_extras: bool) -> None:
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(path for path in stage.rglob("*") if path.is_file()):
            relative = path.relative_to(stage)
            if not include_extras and relative.parts and relative.parts[0] == "extras":
                continue
            archive.write(path, relative.as_posix())


def _zip_winxp(stage: Path, destination: Path, metadata: bytes) -> None:
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in ("spice.exe", "spicecfg.exe"):
            archive.write(stage / "extras" / "winxp" / name, name)
        archive.write(stage / "LICENSE.txt", "LICENSE.txt")
        archive.writestr("FORK_VERSION", (stage / "FORK_VERSION").read_bytes())
        archive.writestr("RELEASE.json", metadata)
        archive.writestr(
            "README.txt",
            "Windows XP package for an existing installation; back up/replace executables, "
            "preserve game files/modules/launch arguments; see docs/Fork-setup.md.\n",
        )


def package_release(repo_root: Path | str, binaries: Path | str, output: Path | str) -> list[Path]:
    """Package binaries using the clean git HEAD at *repo_root*."""
    repo = Path(repo_root).resolve()
    binary_dir = Path(binaries).resolve()
    output_dir = Path(output)
    if output_dir.is_symlink():
        raise PackageError(f"output must be a new empty directory, not a symlink: {output_dir}")
    output_dir = output_dir.resolve()
    if not repo.is_dir():
        raise PackageError(f"repository root does not exist: {repo}")
    if not binary_dir.is_dir():
        raise PackageError(f"binaries directory does not exist: {binary_dir}")
    for relative in (
        "spice.exe", "spice64.exe", "spicecfg.exe",
        "extras/winxp/spice.exe", "extras/winxp/spicecfg.exe",
    ):
        path = binary_dir / relative
        if path.is_symlink() or not path.is_file():
            kind = "Windows XP binary" if relative.startswith("extras/winxp/") else "regular binary"
            raise PackageError(f"missing {kind} {relative}: {path}")
    for path in binary_dir.rglob("*"):
        if path.is_symlink():
            raise PackageError(f"symlinks are not allowed in binary input: {path}")
    try:
        output_dir.relative_to(binary_dir)
    except ValueError:
        pass
    else:
        raise PackageError("output must not be inside the binaries directory")
    if output_dir.exists():
        if not output_dir.is_dir():
            raise PackageError(f"output is not a directory: {output_dir}")
        if any(output_dir.iterdir()):
            raise PackageError(f"output directory is not empty: {output_dir}")
    if _git(repo, "status", "--porcelain=v1", "--untracked-files=all"):
        raise PackageError("working tree has tracked changes or untracked source; commit before packaging")
    source_commit = _git(repo, "rev-parse", "HEAD")
    source_tree = _git(repo, "rev-parse", f"{source_commit}^{{tree}}")
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit) or not re.fullmatch(r"[0-9a-f]{40}", source_tree):
        raise PackageError("unable to resolve a complete source HEAD commit/tree")
    try:
        version = parse_version(_git(repo, "show", f"{source_commit}:FORK_VERSION"))
    except (subprocess.CalledProcessError, ValueError) as exc:
        raise PackageError(str(exc)) from exc
    try:
        upstream_base = _git(repo, "show", f"{source_commit}:UPSTREAM_BASE")
    except subprocess.CalledProcessError as exc:
        raise PackageError(f"unable to read UPSTREAM_BASE: {exc}") from exc
    if not re.fullmatch(r"[0-9a-fA-F]{40}", upstream_base):
        raise PackageError("UPSTREAM_BASE must be a complete 40-hex git commit")
    try:
        upstream_commit = _git(repo, "rev-parse", f"{upstream_base}^{{commit}}")
    except subprocess.CalledProcessError as exc:
        raise PackageError(f"UPSTREAM_BASE is not a commit in this repository: {upstream_base}") from exc
    if upstream_commit.lower() != upstream_base.lower():
        raise PackageError("UPSTREAM_BASE did not resolve to the specified complete commit")
    try:
        _git(repo, "merge-base", "--is-ancestor", upstream_commit, source_commit)
    except subprocess.CalledProcessError as exc:
        raise PackageError("UPSTREAM_BASE is not an ancestor of source HEAD") from exc
    build_source = binary_dir / "src" / f"spice2x-{source_commit}.tar.gz"
    try:
        with tarfile.open(build_source, "r:gz") as archive:
            if archive.pax_headers.get("comment") != source_commit:
                raise PackageError("binary build source does not match source HEAD")
    except (OSError, tarfile.TarError) as exc:
        raise PackageError(f"missing or invalid binary build source: {build_source}") from exc
    metadata = {
        "fork_version": version,
        "source_commit": source_commit,
        "source_tree": source_tree,
        "upstream_version": version.rsplit("-", 1)[0],
        "upstream_commit": upstream_commit.lower(),
    }
    source_name = f"spice2x-{version}-source.tar.gz"
    with tempfile.TemporaryDirectory(prefix="spice2x-release-") as temporary:
        temporary_root = Path(temporary)
        stage = temporary_root / "stage"
        shutil.copytree(binary_dir, stage)
        stage_src = stage / "src"
        if stage_src.is_dir() and not stage_src.is_symlink():
            shutil.rmtree(stage_src)
        elif stage_src.exists() or stage_src.is_symlink():
            stage_src.unlink()
        stage_src.mkdir()

        source_archive = temporary_root / source_name
        with source_archive.open("wb") as stream:
            subprocess.run(
                ["git", "-C", str(repo), "archive", "--format=tar.gz",
                 f"--prefix=spice2x-{version}/", source_commit],
                check=True, stdout=stream, stderr=subprocess.PIPE,
            )
        shutil.copy2(source_archive, stage_src / source_name)
        (stage / "LICENSE.txt").write_bytes(subprocess.check_output(
            ["git", "-C", str(repo), "show", f"{source_commit}:LICENSE"],
        ))
        (stage / "FORK_VERSION").write_text(version + "\n", encoding="utf-8")
        metadata_bytes = (json.dumps(metadata, indent=2) + "\n").encode("utf-8")
        (stage / "RELEASE.json").write_bytes(metadata_bytes)

        package_dir = temporary_root / "output"
        package_dir.mkdir()
        names = [
            f"spice2x-{version}.zip",
            f"spice2x-{version}-full.zip",
            f"spice2x-{version}-winxp.zip",
            source_name,
            "RELEASE.json",
        ]
        _zip_stage(stage, package_dir / names[0], include_extras=False)
        _zip_stage(stage, package_dir / names[1], include_extras=True)
        _zip_winxp(stage, package_dir / names[2], metadata_bytes)
        shutil.copy2(source_archive, package_dir / source_name)
        shutil.copy2(stage / "RELEASE.json", package_dir / "RELEASE.json")
        checksums = "\n".join(
            f"{hashlib.sha256((package_dir / name).read_bytes()).hexdigest()}  {name}"
            for name in sorted(names)
        ) + "\n"
        (package_dir / "SHA256SUMS").write_text(checksums, encoding="ascii")
        output_dir.mkdir(parents=True, exist_ok=True)
        written = []
        for path in sorted(package_dir.iterdir()):
            shutil.copy2(path, output_dir / path.name)
            written.append(output_dir / path.name)
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", required=True, help="existing bin/spice2x directory")
    parser.add_argument(
        "--output", default="build/release",
        help="new empty output directory (default: build/release)",
    )
    args = parser.parse_args(argv)
    repo = Path(__file__).resolve().parents[1]
    binaries = Path(args.binaries)
    output = Path(args.output)
    if not binaries.is_absolute():
        binaries = repo / binaries
    if not output.is_absolute():
        output = repo / output
    try:
        files = package_release(repo, binaries, output)
    except (PackageError, subprocess.CalledProcessError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    for path in files:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
