#!/usr/bin/env python3
"""Exercise the native OTA helper through real Windows processes under Wine."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
REPO = ROOT.parents[1]
BUILD = REPO / "build" / "ota-engine"
TOOLCHAIN = REPO / "build" / "toolchains" / "llvm-mingw-xp-22.1.0"
CXX = Path(os.environ.get("OTA_TEST_CXX", str(TOOLCHAIN / "bin" / "x86_64-w64-mingw32-clang++")))
X86_CXX = TOOLCHAIN / "bin" / "i686-w64-mingw32-clang++"
WINE_DLL_OVERRIDES = "mscoree,mshtml=d"


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, text=True, **kwargs)


def compile_harness(output: Path, behavior: int, x86: bool = False) -> None:
    compiler = X86_CXX if x86 else CXX
    flags = ["-DSPICE_XP=1" if x86 else "-DSPICE_XP=0",
             f"-DOTA_HARNESS_BEHAVIOR={behavior}"]
    if x86:
        flags.append("-DOTA_HARNESS_X86=1")
    run([
        str(compiler), "-std=c++20", *flags,
        "-static", "-static-libgcc", "-static-libstdc++", "-Wall", "-Wextra", "-Werror",
        "-I" + str(ROOT),
        str(ROOT / "api/tests/ota_update_engine_windows_harness.cpp"),
        str(ROOT / "util/ota_update.cpp"),
        str(ROOT / "api/tests/ota_update_engine_signature_stub.cpp"),
        "-ladvapi32", "-o", str(output),
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def wait_for(path: Path, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            return
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {path}")


def run_command(prefix: Path, directory: Path, arguments: list[str], arch: str = "win64",
        executable: str = "spice64.exe") -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment.update({"WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEARCH": arch,
                       "WINEDLLOVERRIDES": WINE_DLL_OVERRIDES})
    return subprocess.run(
        ["wine", str(directory / executable), *arguments],
        cwd=directory, env=environment, text=True, capture_output=True, timeout=90,
        check=False,
    )


def run_case(prefix: Path, directory: Path, payload: Path,
        config: Path | None = None) -> subprocess.CompletedProcess[str]:
    arguments = ["--apply", str(payload)]
    if config:
        arguments.append(str(config))
    return run_command(prefix, directory, arguments)


def staged_directories(directory: Path) -> list[Path]:
    return [path for path in directory.glob(".spice2x-ota-*")
            if not path.name.startswith(".spice2x-ota-helper")]


def wait_for_no_staging(directory: Path, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not staged_directories(directory):
            return
        time.sleep(0.1)
    details = [str(path.relative_to(directory)) for stage in staged_directories(directory)
            for path in stage.rglob("*")]
    raise AssertionError(f"OTA staging directory remains: {staged_directories(directory)} ({details})")


def assert_one_fixed_helper(directory: Path) -> None:
    helpers = sorted(directory.glob(".spice2x-ota-helper*"))
    assert helpers == [directory / ".spice2x-ota-helper.exe"], helpers


def read_update_id(directory: Path) -> str:
    value = (directory / "success.update-id").read_text().strip()
    if len(value) != 32 or any(character not in "0123456789abcdefABCDEF" for character in value):
        raise AssertionError(f"invalid update child ID: {value!r}")
    return value


def stop_wine(prefix: Path, arch: str = "win64") -> None:
    environment = os.environ.copy()
    environment.update({"WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEARCH": arch,
                       "WINEDLLOVERRIDES": WINE_DLL_OVERRIDES})
    subprocess.run(["wineserver", "-k"], env=environment, check=False,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        subprocess.run(["wineserver", "-w"], env=environment, check=False,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    except subprocess.TimeoutExpired:
        pass


def main() -> int:
    if not CXX.exists() or shutil.which("wine") is None:
        print(f"SKIP: native OTA Wine harness requires {CXX} and wine")
        return 0

    BUILD.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wine-", dir=BUILD) as temporary:
        root = Path(temporary)
        parent = root / "parent.exe"
        success = root / "success.exe"
        failure = root / "failure.exe"
        x86_parent = root / "x86-parent.exe"
        compile_harness(parent, 0)
        compile_harness(success, 1)
        compile_harness(failure, 2)
        if X86_CXX.exists():
            compile_harness(x86_parent, 0, x86=True)

        prefix = root / "wine-prefix"
        environment = os.environ.copy()
        try:
            environment.update({"WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEARCH": "win64",
                               "WINEDLLOVERRIDES": WINE_DLL_OVERRIDES})
            try:
                subprocess.run(["wineboot", "-u"], env=environment, check=True,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                print("SKIP: isolated Wine prefix could not initialize")
                return 0

            success_case = root / "success-case"
            success_case.mkdir()
            active = success_case / "spice64.exe"
            success_payload = success_case / "success-payload.exe"
            config_payload = success_case / "config-payload.exe"
            shutil.copy2(parent, active)
            shutil.copy2(success, success_payload)
            if x86_parent.exists():
                shutil.copy2(x86_parent, config_payload)
            parent_bytes = active.read_bytes()
            success_bytes = success_payload.read_bytes()
            result = run_case(prefix, success_case, success_payload,
                    config_payload if config_payload.exists() else None)
            if result.returncode != 0 and not (success_case / "success.ready").is_file():
                raise AssertionError(
                        f"success harness failed ({result.returncode}): {result.stderr}")
            wait_for(success_case / "success.ready")
            first_update_id = read_update_id(success_case)
            wait_for(success_case / "stale.cleaned")
            wait_for_no_staging(success_case)
            assert active.read_bytes() == success_bytes
            assert (success_case / ".spice2x-previous-spice64.exe").read_bytes() == parent_bytes
            if config_payload.exists():
                assert (success_case / "spicecfg.exe").read_bytes() == config_payload.read_bytes()
            assert not staged_directories(success_case), result.stderr
            assert_one_fixed_helper(success_case)

            # Reapply from the same directory. The previous helper is reused and the
            # prior installed executable remains available as the one-version backup.
            (success_case / "success.ready").unlink()
            (success_case / "success.update-id").unlink()
            (success_case / "stale.cleaned").unlink()
            second_payload = success_case / "second-payload.exe"
            shutil.copy2(parent, second_payload)
            result = run_case(prefix, success_case, second_payload,
                    config_payload if config_payload.exists() else None)
            if result.returncode != 0 and not (success_case / "success.ready").is_file():
                raise AssertionError(
                        f"repeated success harness failed ({result.returncode}): {result.stderr}")
            wait_for(success_case / "success.ready")
            second_update_id = read_update_id(success_case)
            if second_update_id == first_update_id:
                raise AssertionError("repeated OTA update reused its opaque ID")
            wait_for(success_case / "stale.cleaned")
            wait_for_no_staging(success_case)
            assert active.read_bytes() == parent_bytes
            assert (success_case / ".spice2x-previous-spice64.exe").read_bytes() == success_bytes
            if config_payload.exists():
                assert (success_case / "spicecfg.exe").read_bytes() == config_payload.read_bytes()
                assert (success_case / ".spice2x-previous-spicecfg.exe").read_bytes() == \
                        config_payload.read_bytes()
            assert_one_fixed_helper(success_case)

            lifecycle_case = root / "lifecycle-case"
            lifecycle_case.mkdir()
            shutil.copy2(parent, lifecycle_case / "spice64.exe")
            result = run_command(prefix, lifecycle_case, ["--lifecycle"])
            if result.returncode != 0:
                raise AssertionError(
                        f"lifecycle harness failed ({result.returncode}): {result.stderr}")
            wait_for(lifecycle_case / "lifecycle.ready")
            wait_for(lifecycle_case / "stale.cleaned")
            wait_for_no_staging(lifecycle_case)
            assert not list(lifecycle_case.glob(".spice2x-ota-helper*"))

            cancel_failure_case = root / "cancel-failure-case"
            cancel_failure_case.mkdir()
            shutil.copy2(parent, cancel_failure_case / "spice64.exe")
            result = run_command(prefix, cancel_failure_case, ["--cancel-failure"])
            if result.returncode != 0:
                raise AssertionError(
                        f"cancel failure harness failed ({result.returncode}): {result.stderr}")
            wait_for(cancel_failure_case / "cancel-failure.ready")
            wait_for_no_staging(cancel_failure_case)
            assert not list(cancel_failure_case.glob(".spice2x-ota-helper*"))

            failure_case = root / "failure-case"
            failure_case.mkdir()
            active = failure_case / "spice64.exe"
            failure_payload = failure_case / "failure-payload.exe"
            shutil.copy2(parent, active)
            shutil.copy2(failure, failure_payload)
            parent_bytes = active.read_bytes()
            result = run_case(prefix, failure_case, failure_payload)
            if result.returncode != 0 and not (failure_case / "failure.started").is_file():
                raise AssertionError(
                        f"failure harness failed ({result.returncode}): {result.stderr}")
            wait_for(failure_case / "failure.started")
            wait_for(failure_case / "rollback.ready")
            wait_for(failure_case / "stale.cleaned")
            wait_for_no_staging(failure_case)
            assert active.read_bytes() == parent_bytes
            assert not staged_directories(failure_case), result.stderr

            if x86_parent.exists():
                x86_prefix = root / "wine-prefix-x86"
                x86_environment = os.environ.copy()
                x86_environment.update({"WINEPREFIX": str(x86_prefix), "WINEDEBUG": "-all",
                                        "WINEARCH": "win64",
                                        "WINEDLLOVERRIDES": WINE_DLL_OVERRIDES})
                subprocess.run(["wineboot", "-u"], env=x86_environment, check=True,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
                x86_case = root / "x86-lifecycle-case"
                x86_case.mkdir()
                shutil.copy2(x86_parent, x86_case / "spice.exe")
                result = run_command(x86_prefix, x86_case, ["--lifecycle"],
                        arch="win64", executable="spice.exe")
                if result.returncode != 0:
                    raise AssertionError(
                            f"x86 lifecycle harness failed ({result.returncode}): {result.stderr}")
                wait_for(x86_case / "lifecycle.ready")
                wait_for(x86_case / "stale.cleaned")
                wait_for_no_staging(x86_case)
                assert not list(x86_case.glob(".spice2x-ota-helper*"))

                x86_cancel_failure_case = root / "x86-cancel-failure-case"
                x86_cancel_failure_case.mkdir()
                shutil.copy2(x86_parent, x86_cancel_failure_case / "spice.exe")
                result = run_command(x86_prefix, x86_cancel_failure_case, ["--cancel-failure"],
                        arch="win64", executable="spice.exe")
                if result.returncode != 0:
                    raise AssertionError(
                            f"x86 cancel failure harness failed ({result.returncode}): "
                            f"{result.stderr}")
                wait_for(x86_cancel_failure_case / "cancel-failure.ready")
                wait_for_no_staging(x86_cancel_failure_case)
                assert not list(x86_cancel_failure_case.glob(".spice2x-ota-helper*"))
        finally:
            stop_wine(prefix)
            if (root / "wine-prefix-x86").exists():
                stop_wine(root / "wine-prefix-x86", arch="win64")

    print("OTA Wine engine tests passed: helper handshake, replacement, retained backup, "
          "rollback and x86 cancellation.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
