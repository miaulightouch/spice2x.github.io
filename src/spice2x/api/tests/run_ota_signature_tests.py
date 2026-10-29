#!/usr/bin/env python3
"""Compile the Authenticode verifier for the XP toolchain and exercise it under Wine."""

from __future__ import annotations

import hashlib
import base64
import os
from pathlib import Path
import re
import shutil
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


def public_key_pin(certificate: Path) -> str:
    public_key = subprocess.check_output([
        "openssl", "x509", "-in", str(certificate), "-pubkey", "-noout",
    ], stderr=subprocess.DEVNULL)
    der = subprocess.check_output(
        ["openssl", "pkey", "-pubin", "-outform", "DER"],
        input=public_key, stderr=subprocess.DEVNULL,
    )
    return hashlib.sha1(der).hexdigest()


def public_key_pin_header(certificate: Path, include_root: Path) -> Path:
    digest = bytes.fromhex(public_key_pin(certificate))
    header = include_root / "build" / "ota_public_key.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    values = ", ".join(f"0x{byte:02x}" for byte in digest)
    header.write_text(
        "#pragma once\n"
        "#include <array>\n"
        "#include <cstdint>\n"
        "namespace ota {\n"
        f"inline constexpr std::array<std::uint8_t, 20> kPinnedPublicKeySha1 = {{{values}}};\n"
        "}\n"
    )
    return include_root


def make_pfx(certificate: Path, key: Path, destination: Path, password: str) -> None:
    password_file = destination.with_name(f".{destination.name}.password")
    password_file.write_text(password + "\n")
    password_file.chmod(stat.S_IRUSR | stat.S_IWUSR)
    try:
        run([
            "openssl", "pkcs12", "-export", "-inkey", str(key), "-in", str(certificate),
            "-out", str(destination), "-passout", f"file:{password_file}",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    finally:
        password_file.unlink(missing_ok=True)
    destination.chmod(stat.S_IRUSR | stat.S_IWUSR)


def sign_with_osslsigncode(signer: Path, source: Path, destination: Path,
                           pfx: Path, password: str) -> None:
    destination.unlink(missing_ok=True)
    password_file = destination.with_name(f".{destination.name}.password")
    password_file.write_text(password + "\n")
    password_file.chmod(stat.S_IRUSR | stat.S_IWUSR)
    try:
        run([
            str(signer), "sign", "-pkcs12", str(pfx), "-readpass", str(password_file),
            "-h", "sha1", "-in", str(source), "-out", str(destination),
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    finally:
        password_file.unlink(missing_ok=True)


def assert_release_pin_matches_script() -> None:
    header = (ROOT / "build/ota_public_key.h").read_text()
    array = re.search(r"kPinnedPublicKeySha1\s*=\s*\{([^}]*)\}", header, re.S)
    script = re.search(r"pinned_key_sha1=([0-9a-fA-F]+)", (ROOT / "sign_release.sh").read_text())
    assert array and script
    pin = "".join(re.findall(r"0x([0-9a-fA-F]{2})", array.group(1))).lower()
    assert len(pin) == 40 and pin == script.group(1).lower()


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


def test_release_archive_update(signed: Path, certificate: Path, key: Path,
                                pfx: Path, password: str) -> None:
    with tempfile.TemporaryDirectory(prefix="release-script-", dir=BUILD) as temporary:
        sandbox = Path(temporary)
        script_root = sandbox / "src" / "spice2x"
        output_root = script_root / "bin" / "spice2x"
        dist = script_root / "dist"
        fake_bin = sandbox / "fake-bin"
        script_root.mkdir(parents=True)
        dist.mkdir()
        fake_bin.mkdir()
        shutil.copy2(ROOT / "sign_release.sh", script_root / "sign_release.sh")
        script = script_root / "sign_release.sh"
        script_text = script.read_text()
        script_text, replacements = re.subn(
            r"(?m)^pinned_key_sha1=[0-9a-fA-F]+$",
            f"pinned_key_sha1={public_key_pin(certificate)}",
            script_text,
        )
        assert replacements == 1
        script.write_text(script_text)
        script.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)

        windows_files = [
            output_root / "spice.exe",
            output_root / "spice64.exe",
            output_root / "spicecfg.exe",
            output_root / "extras" / "largeaddressaware" / "spice.exe",
            output_root / "extras" / "winxp" / "spice.exe",
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
            "if [[ \"$1\" == sign && \"$2\" == --help ]]; then echo -readpass; exit 0; fi\n"
            "input= output=\n"
            "while (($#)); do case $1 in -in) input=$2; shift 2;; -out) output=$2; shift 2;; *) shift;; esac; done\n"
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

        environment = {
            **os.environ,
            "PATH": str(fake_bin) + os.pathsep + os.environ.get("PATH", ""),
            "OTA_SIGNING_PFX": base64.b64encode(pfx.read_bytes()).decode("ascii"),
            "OTA_SIGNING_PASSWORD": password,
        }
        run([str(script)], cwd=script_root, env=environment)

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

        missing_password = dict(environment)
        del missing_password["OTA_SIGNING_PASSWORD"]
        failed = subprocess.run(
            [str(script)], cwd=script_root,
            env=missing_password, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        assert failed.returncode != 0

        wrong_password = dict(environment)
        wrong_password["OTA_SIGNING_PASSWORD"] = "wrong-password"
        failed = subprocess.run(
            [str(script)], cwd=script_root,
            env=wrong_password, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        assert failed.returncode != 0

        empty_pfx = sandbox / "empty-password.pfx"
        run([
            "openssl", "pkcs12", "-export", "-inkey", str(key),
            "-in", str(certificate), "-out", str(empty_pfx), "-passout", "pass:",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        empty_environment = dict(environment)
        empty_environment["OTA_SIGNING_PFX"] = base64.b64encode(empty_pfx.read_bytes()).decode("ascii")
        empty_environment["OTA_SIGNING_PASSWORD"] = ""
        run([str(script)], cwd=script_root, env=empty_environment)


def main() -> int:
    if not CXX.exists():
        print(f"SKIP: XP cross compiler unavailable: {CXX}")
        return 0
    if not shutil.which("openssl"):
        print("SKIP: openssl is required for genuine Authenticode fixtures")
        return 0

    os.umask(0o077)
    BUILD.mkdir(parents=True, exist_ok=True)
    assert_release_pin_matches_script()
    signer = Path(os.environ.get(
        "OTA_TEST_SIGNER", str(BUILD / "osslsigncode-build" / "osslsigncode")
    ))
    with tempfile.TemporaryDirectory(prefix="fixture-", dir=BUILD) as temporary:
        fixture = Path(temporary)
        cert, key = make_certificate(fixture, "/C=TW/O=Fixture/CN=OTA Fixture")
        pfx = fixture / "signing.pfx"
        make_pfx(cert, key, pfx, "fixture-password")
        include_root = public_key_pin_header(cert, fixture / "include")
        executable = compile_test(include_root)
        unsigned = BUILD / "unsigned.exe"
        shutil.copy2(executable, unsigned)
        command = ["wine", str(executable), str(unsigned), "unsigned"]

        if not signer.is_file():
            print("XP compile passed; osslsigncode unavailable; genuine signed runtime skipped")
            run_wine(command)
            return 0

        signed = BUILD / "signed-real.exe"
        sign_with_osslsigncode(signer, unsigned, signed, pfx, "fixture-password")
        test_release_archive_update(signed, cert, key, pfx, "fixture-password")
        mutated_code = BUILD / "mutated-code.exe"
        mutated_pkcs7 = BUILD / "mutated-pkcs7.exe"
        mutate_code(signed, mutated_code)
        mutate_pkcs7(signed, mutated_pkcs7)

        wrong_fixture = fixture / "wrong"
        wrong_fixture.mkdir()
        wrong_cert, wrong_key = make_certificate(
            wrong_fixture, "/C=TW/O=Other/CN=Other OTA Test"
        )
        wrong_pfx = wrong_fixture / "signing.pfx"
        make_pfx(wrong_cert, wrong_key, wrong_pfx, "wrong-password")
        wrong = BUILD / "wrong-signer.exe"
        sign_with_osslsigncode(signer, unsigned, wrong, wrong_pfx, "wrong-password")
        command.extend([
            str(signed), "verified", str(mutated_code), "invalid",
            str(mutated_pkcs7), "invalid", str(wrong), "untrusted",
        ])
        print("XP compile passed; Wine runtime below is not actual Windows XP")
        run_wine(command)
    return 0


if __name__ == "__main__":
    sys.exit(main())
