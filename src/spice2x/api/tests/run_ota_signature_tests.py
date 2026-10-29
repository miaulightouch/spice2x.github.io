#!/usr/bin/env python3
"""Compile the Authenticode verifier for the XP toolchain and exercise it under Wine."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import shlex
import stat
import struct
import subprocess
import sys
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile
import zlib


ROOT = Path(__file__).resolve().parents[2]
REPO = ROOT.parents[1]
BUILD = REPO / "build" / "ota-signature"
TOOLCHAIN = REPO / "build" / "toolchains" / "llvm-mingw-xp-22.1.0"
CXX = Path(os.environ.get(
    "OTA_TEST_CXX",
    str(TOOLCHAIN / "bin" / "i686-w64-mingw32-clang++"),
))


def run(command: list[str], **kwargs: object) -> None:
    subprocess.run(command, check=True, **kwargs)


def run_wine(command: list[str]) -> None:
    if os.environ.get("OTA_TEST_SKIP_WINE"):
        print("XP compile passed; Wine runtime explicitly skipped; no actual Windows XP result")
        return
    if not shutil.which("wine"):
        print("XP compile passed; Wine is unavailable; no actual Windows XP result")
        return
    wine_prefix = Path(os.environ.get(
        "OTA_TEST_WINEPREFIX", str(REPO / "local" / "ota-signing" / "wine-prefix")
    ))
    wine_prefix.mkdir(parents=True, exist_ok=True)
    wine_prefix.chmod(stat.S_IRWXU)
    wine_environment = {
        **os.environ,
        "WINEDEBUG": "-all",
        "WINEPREFIX": str(wine_prefix),
        "WINEDLLOVERRIDES": "mscoree,mshtml=d",
    }
    try:
        subprocess.run(
            command, check=True, timeout=int(os.environ.get("OTA_TEST_WINE_TIMEOUT", "60")),
            env=wine_environment,
        )
    except subprocess.TimeoutExpired:
        print("XP compile passed; Wine runtime did not start within the timeout", file=sys.stderr)
        raise
    finally:
        for wineserver_command in (("wineserver", "-k"), ("wineserver", "-w")):
            try:
                subprocess.run(
                    wineserver_command, env=wine_environment, check=False,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10,
                )
            except (subprocess.TimeoutExpired, FileNotFoundError):
                pass


def certificate_table(image: bytes) -> tuple[int, int]:
    if image[:2] != b"MZ":
        raise AssertionError("fixture is not a PE image")
    nt = struct.unpack_from("<I", image, 0x3C)[0]
    optional = nt + 4 + 20
    magic = struct.unpack_from("<H", image, optional)[0]
    directories = optional + (96 if magic == 0x10B else 112)
    return struct.unpack_from("<II", image, directories + 4 * 8)


def mutate_code(source: Path, destination: Path) -> None:
    image = bytearray(source.read_bytes())
    image[0x1000] ^= 0x40
    destination.write_bytes(image)


def mutate_pkcs7(source: Path, destination: Path) -> None:
    image = bytearray(source.read_bytes())
    offset, size = certificate_table(image)
    if not size or offset + size > len(image):
        raise AssertionError("signed fixture has no certificate table")
    image[offset + 8 + 17] ^= 0x01
    destination.write_bytes(image)


def make_certificate(directory: Path, subject: str) -> tuple[Path, Path]:
    key = directory / "key.pem"
    cert = directory / "cert.pem"
    run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha1", "-nodes",
        "-days", "3650", "-subj", subject,
        "-keyout", str(key), "-out", str(cert),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    key.chmod(stat.S_IRUSR | stat.S_IWUSR)
    cert.chmod(stat.S_IRUSR | stat.S_IWUSR)
    return cert, key


def write_env_file(path: Path, key: str | None, certificate: str | None) -> None:
    values = []
    if key is not None:
        values.append(f"OTA_SIGNING_KEY={shlex.quote(key)}")
    if certificate is not None:
        values.append(f"OTA_SIGNING_CERT={shlex.quote(certificate)}")
    path.write_text("\n".join(values) + "\n")


def clean_signing_environment() -> dict[str, str]:
    environment = dict(os.environ)
    for name in ("OTA_SIGNING_KEY", "OTA_SIGNING_CERT"):
        environment.pop(name, None)
    return environment


def public_key_pin(certificate: Path) -> str:
    public_key = subprocess.check_output([
        "openssl", "x509", "-in", str(certificate), "-pubkey", "-noout",
    ], stderr=subprocess.DEVNULL)
    der = subprocess.check_output(
        ["openssl", "pkey", "-pubin", "-outform", "DER"],
        input=public_key, stderr=subprocess.DEVNULL,
    )
    return hashlib.sha1(der).hexdigest()


def sign_with_osslsigncode(signer: Path, source: Path, destination: Path,
                           certificate: Path, key: Path) -> None:
    destination.unlink(missing_ok=True)
    run([
        str(signer), "sign", "-key", str(key), "-certs", str(certificate), "-pass", "",
        "-h", "sha1", "-in", str(source), "-out", str(destination),
    ], stdout=subprocess.DEVNULL)


def header_pin(header: Path) -> str:
    array = re.search(r"kPinnedPublicKeySha1\s*=\s*\{([^}]*)\}",
                      header.read_text(), re.S)
    assert array
    pin = "".join(re.findall(r"0x([0-9a-fA-F]{2})", array.group(1))).lower()
    if not pin.strip():
        return "0" * 40
    assert len(pin) == 40
    return pin


def copy_script_root(sandbox: Path) -> Path:
    script_root = sandbox / "src" / "spice2x"
    script_root.mkdir(parents=True)
    shutil.copy2(ROOT / "sign_release.sh", script_root / "sign_release.sh")
    (script_root / "build").mkdir()
    shutil.copy2(ROOT / "build/ota_public_key.h", script_root / "build/ota_public_key.h")
    script = script_root / "sign_release.sh"
    script.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
    return script_root


def prepare_header(script_root: Path, key: str | None, certificate: str | None,
                   environment: dict[str, str] | None = None) -> Path:
    env_file = script_root.parents[1] / ".env.local"
    if key is not None or certificate is not None:
        write_env_file(env_file, key, certificate)
    environment = environment or clean_signing_environment()
    run([str(script_root / "sign_release.sh"), "--prepare"], cwd=BUILD, env=environment)
    return script_root / "build" / "ota_public_key.local.h"


def compile_test(include_root: Path) -> Path:
    executable = BUILD / "ota_signature_test.exe"
    command = [
        str(CXX), "-std=c++20", "-DSPICE_XP=1", "-static", "-static-libgcc",
        "-static-libstdc++", "-I" + str(include_root), "-I" + str(ROOT),
        str(ROOT / "api/tests/ota_signature_test.cpp"),
        str(ROOT / "util/ota_signature.cpp"),
        str(ROOT / "external/hash-library/sha1.cpp"),
        str(ROOT / "external/hash-library/sha256.cpp"),
        "-lcrypt32", "-o", str(executable),
    ]
    run(command, stdout=subprocess.DEVNULL)
    return executable


def test_prepare_configuration(certificate: Path, key: Path) -> None:
    key_text = key.read_text()
    certificate_text = certificate.read_text()
    expected = public_key_pin(certificate)
    with tempfile.TemporaryDirectory(prefix="prepare-script-", dir=BUILD) as temporary:
        sandbox = Path(temporary)

        from_file = copy_script_root(sandbox / "from-file")
        generated = prepare_header(from_file, key_text, certificate_text)
        assert header_pin(generated) == expected

        from_environment = copy_script_root(sandbox / "from-environment")
        environment = clean_signing_environment()
        environment.update({"OTA_SIGNING_KEY": key_text, "OTA_SIGNING_CERT": certificate_text})
        generated = prepare_header(from_environment, None, None, environment)
        assert header_pin(generated) == expected

        empty = copy_script_root(sandbox / "empty")
        generated = prepare_header(empty, "", "")
        assert header_pin(generated) == "0" * 40

        missing = copy_script_root(sandbox / "missing")
        generated = prepare_header(missing, None, None)
        assert header_pin(generated) == "0" * 40
        skipped = subprocess.run(
            [str(missing / "sign_release.sh")], cwd=BUILD,
            env=clean_signing_environment(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        assert skipped.returncode == 0

        for name, value in (("OTA_SIGNING_KEY", key_text), ("OTA_SIGNING_CERT", certificate_text)):
            partial = copy_script_root(sandbox / f"partial-{name.lower()}")
            partial_environment = clean_signing_environment()
            partial_environment[name] = value
            failed = subprocess.run(
                [str(partial / "sign_release.sh")], cwd=BUILD, env=partial_environment,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            assert failed.returncode != 0


def test_release_archive_update(signed: Path, certificate: Path, key: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="release-script-", dir=BUILD) as temporary:
        sandbox = Path(temporary)
        script_root = copy_script_root(sandbox)
        output_root = script_root / "bin" / "spice2x"
        dist = script_root / "dist"
        fake_bin = sandbox / "fake-bin"
        dist.mkdir()
        fake_bin.mkdir()
        script = script_root / "sign_release.sh"

        windows_files = [
            output_root / "spice.exe",
            output_root / "spice64.exe",
            output_root / "spicecfg.exe",
            output_root / "extras" / "largeaddressaware" / "spice.exe",
            output_root / "extras" / "winxp" / "spice.exe",
            output_root / "extras" / "winxp" / "spice64.exe",
            output_root / "extras" / "winxp" / "spicecfg.exe",
            output_root / "extras" / "linux" / "spice.exe",
            output_root / "extras" / "linux" / "spice64.exe",
            output_root / "extras" / "linux" / "spicecfg.exe",
        ]
        for path in windows_files:
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(signed, path)

        fake_signer = fake_bin / "osslsigncode"
        fake_signer.write_text(
            "#!/usr/bin/env bash\n"
            "set -eu\n"
            "key= certs= pass= input= output= pass_seen=0\n"
            "while (($#)); do\n"
            "    case $1 in\n"
            "        -key) key=$2; shift 2;;\n"
            "        -certs) certs=$2; shift 2;;\n"
            "        -pass) pass_seen=1; pass=$2; shift 2;;\n"
            "        -in) input=$2; shift 2;;\n"
            "        -out) output=$2; shift 2;;\n"
            "        -h) shift 2;;\n"
            "        *) shift;;\n"
            "    esac\n"
            "done\n"
            "[[ -n \"$key\" && -n \"$certs\" && -n \"$input\" && -n \"$output\" ]]\n"
            "[[ \"$pass_seen\" == 1 && -z \"$pass\" ]]\n"
            "[[ -f \"$key\" && -f \"$certs\" ]]\n"
            "grep -q 'BEGIN .*PRIVATE KEY' \"$key\"\n"
            "grep -q 'BEGIN CERTIFICATE' \"$certs\"\n"
            "printf '%s|%s|%s\n' \"$pass_seen\" \"$key\" \"$certs\" >> \"$OTA_TEST_SIGNER_LOG\"\n"
            "[[ -n \"$input\" && -n \"$output\" ]] && cp -p \"$input\" \"$output\"\n",
        )
        fake_signer.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)

        standard_entries = {
            "spice.exe": b"old normal 32",
            "spice64.exe": b"old normal 64",
            "spicecfg.exe": b"old config",
            "README.txt": b"keep this",
        }
        full_entries = {
            **standard_entries,
            "extras/largeaddressaware/spice.exe": b"old laa",
            "extras/winxp/spice.exe": b"old xp 32",
            "extras/winxp/spice64.exe": b"old xp 64",
            "extras/winxp/spicecfg.exe": b"old xp config",
            "extras/linux/spice.exe": b"old linux 32",
            "extras/linux/spice64.exe": b"old linux 64",
            "extras/linux/spicecfg.exe": b"old linux config",
        }
        archives = {"normal.zip": standard_entries, "full.zip": full_entries}
        archive_comments = {
            "normal.zip": b"normal release comment",
            "full.zip": b"full release comment",
        }
        original_names = {}
        for name, entries in archives.items():
            archive = dist / name
            with ZipFile(archive, "w", ZIP_DEFLATED) as output:
                for entry, contents in entries.items():
                    output.writestr(entry, contents)
                output.comment = archive_comments[name]
            with ZipFile(archive) as source:
                original_names[name] = source.namelist()

        signer_log = sandbox / "signer.log"
        environment = clean_signing_environment()
        environment.update({
            "PATH": str(fake_bin) + os.pathsep + os.environ.get("PATH", ""),
            "OTA_SIGNING_KEY": key.read_text(),
            "OTA_SIGNING_CERT": certificate.read_text(),
            "OTA_TEST_SIGNER_LOG": str(signer_log),
        })
        run([str(script)], cwd=script_root, env=environment)
        signer_calls = signer_log.read_text().splitlines()
        assert len(signer_calls) == len(windows_files)
        assert all(call.startswith("1|") for call in signer_calls)

        expected = signed.read_bytes()
        for name, entries in archives.items():
            with ZipFile(dist / name) as archive:
                assert archive.testzip() is None
                assert archive.namelist() == original_names[name]
                assert archive.comment == archive_comments[name]
                if name == "full.zip":
                    assert {entry for entry in archive.namelist() if entry.endswith(".exe")} == {
                        entry for entry in entries if entry.endswith(".exe")
                    }
                for entry in entries:
                    data = archive.read(entry)
                    if entry.endswith(".exe"):
                        assert data == expected, (name, entry)
                    else:
                        assert data == entries[entry], (name, entry)
                    info = archive.getinfo(entry)
                    assert info.file_size == len(data)
                    assert info.CRC == zlib.crc32(data) & 0xffffffff

        before = [path.read_bytes() for path in windows_files]
        missing_environment = clean_signing_environment()
        missing_environment.update({
            "PATH": environment["PATH"],
            "OTA_TEST_SIGNER_LOG": str(signer_log),
        })
        run([str(script)], cwd=script_root, env=missing_environment)
        assert [path.read_bytes() for path in windows_files] == before

        for name, value in (("OTA_SIGNING_KEY", key.read_text()),
                            ("OTA_SIGNING_CERT", certificate.read_text())):
            partial_environment = clean_signing_environment()
            partial_environment.update({
                "PATH": environment["PATH"],
                "OTA_TEST_SIGNER_LOG": str(signer_log),
                name: value,
            })
            failed = subprocess.run(
                [str(script)], cwd=script_root, env=partial_environment,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            assert failed.returncode != 0
            assert [path.read_bytes() for path in windows_files] == before


def main() -> int:
    if not CXX.exists():
        print(f"SKIP: XP cross compiler unavailable: {CXX}")
        return 0
    if not shutil.which("openssl"):
        print("SKIP: openssl is required for genuine Authenticode fixtures")
        return 0

    os.umask(0o077)
    BUILD.mkdir(parents=True, exist_ok=True)
    signer = Path(os.environ.get(
        "OTA_TEST_SIGNER", str(BUILD / "osslsigncode-build" / "osslsigncode")
    ))
    with tempfile.TemporaryDirectory(prefix="fixture-", dir=BUILD) as temporary:
        fixture = Path(temporary)
        cert, key = make_certificate(fixture, "/C=TW/O=Fixture/CN=OTA Fixture")
        include_root = copy_script_root(fixture)
        generated = prepare_header(include_root, key.read_text(), cert.read_text())
        assert header_pin(generated) == public_key_pin(cert)
        executable = compile_test(include_root)
        unsigned = BUILD / "unsigned.exe"
        shutil.copy2(executable, unsigned)
        command = ["wine", str(executable), str(unsigned), "unsigned"]
        test_prepare_configuration(cert, key)

        if not signer.is_file():
            print("XP compile passed; osslsigncode unavailable; genuine signed runtime skipped")
            run_wine(command)
            return 0

        signed = BUILD / "signed-real.exe"
        sign_with_osslsigncode(signer, unsigned, signed, cert, key)
        test_release_archive_update(signed, cert, key)
        mutated_code = BUILD / "mutated-code.exe"
        mutated_pkcs7 = BUILD / "mutated-pkcs7.exe"
        mutate_code(signed, mutated_code)
        mutate_pkcs7(signed, mutated_pkcs7)

        wrong_fixture = fixture / "wrong"
        wrong_fixture.mkdir()
        wrong_cert, wrong_key = make_certificate(
            wrong_fixture, "/C=TW/O=Other/CN=Other OTA Test"
        )
        wrong = BUILD / "wrong-signer.exe"
        sign_with_osslsigncode(signer, unsigned, wrong, wrong_cert, wrong_key)
        command.extend([
            str(signed), "verified", str(mutated_code), "invalid",
            str(mutated_pkcs7), "invalid", str(wrong), "untrusted",
        ])
        print("XP compile passed; Wine runtime below is not actual Windows XP")
        run_wine(command)
    return 0


if __name__ == "__main__":
    sys.exit(main())
