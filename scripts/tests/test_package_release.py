import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_release import PackageError, package_release


class PackageReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="spice2x-package-test-")
        self.work = Path(self.temp.name)
        self.repo = self.work / "repo"
        self.repo.mkdir()
        self._git("init", "-q", "-b", "main")
        self._git("config", "user.name", "Package Test")
        self._git("config", "user.email", "package-test@example.invalid")
        self._write("FORK_VERSION", "26-10-03-1\n")
        self._write("LICENSE", "fixture license\n")
        self._write(".gitignore", "local/\n")
        self._write("controller/app.js", "console.log('controller');\n")
        self._write("src/spice2x/main.cpp", "int main() { return 0; }\n")
        self._write("docs/guide.md", "fixture docs\n")
        self._write(".github/workflows/ci.yml", "name: fixture\n")
        self._write("local/ignored.txt", "runtime secret\n")
        self._git("add", ".")
        self._git("commit", "-qm", "upstream fixture")
        self.upstream_commit = self._git("rev-parse", "HEAD")
        self._write("UPSTREAM_BASE", self.upstream_commit + "\n")
        self._git("add", "UPSTREAM_BASE")
        self._git("commit", "-qm", "fork fixture")
        self.source_commit = self._git("rev-parse", "HEAD")
        self.source_tree = self._git("rev-parse", "HEAD^{tree}")

        self.binaries = self.work / "bin" / "spice2x"
        for name in ("spice.exe", "spice64.exe", "spicecfg.exe"):
            self._binary(name)
        self._binary("extras/winxp/spice.exe")
        self._binary("extras/winxp/spicecfg.exe")
        self._binary("extras/keep.dll")
        self._binary("src/old-source.tar.gz")
        self.build_source = self.binaries / "src" / f"spice2x-{self.source_commit}.tar.gz"
        self._git("archive", "--format=tar.gz", f"--output={self.build_source}", "HEAD")

    def tearDown(self):
        self.temp.cleanup()

    def _git(self, *args):
        result = subprocess.run(
            ["git", "-C", str(self.repo), *args],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    def _write(self, relative, text):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def _binary(self, relative):
        path = self.binaries / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(("fixture " + relative).encode("ascii"))

    def _package(self):
        output = self.work / "release"
        package_release(self.repo, self.binaries, output)
        return output

    def test_archives_source_metadata_and_checksums(self):
        output = self._package()
        version = "26-10-03-1"
        expected = {
            f"spice2x-{version}.zip",
            f"spice2x-{version}-full.zip",
            f"spice2x-{version}-winxp.zip",
            f"spice2x-{version}-source.tar.gz",
            "RELEASE.json",
            "SHA256SUMS",
        }
        self.assertEqual({path.name for path in output.iterdir()}, expected)

        metadata = json.loads((output / "RELEASE.json").read_text(encoding="utf-8"))
        self.assertEqual(
            metadata,
            {
                "fork_version": version,
                "source_commit": self.source_commit,
                "source_tree": self.source_tree,
                "upstream_version": "26-10-03",
                "upstream_commit": self.upstream_commit,
            },
        )

        with zipfile.ZipFile(output / f"spice2x-{version}.zip") as archive:
            names = set(archive.namelist())
            self.assertIn("spice.exe", names)
            self.assertIn("spice64.exe", names)
            self.assertIn("spicecfg.exe", names)
            self.assertIn("LICENSE.txt", names)
            self.assertIn("FORK_VERSION", names)
            self.assertIn("RELEASE.json", names)
            self.assertIn(f"src/spice2x-{version}-source.tar.gz", names)
            self.assertFalse(any(name.startswith("extras/") for name in names))

        with zipfile.ZipFile(output / f"spice2x-{version}-full.zip") as archive:
            names = set(archive.namelist())
            self.assertIn("extras/keep.dll", names)
            self.assertIn("extras/winxp/spice.exe", names)
            self.assertIn("extras/winxp/spicecfg.exe", names)
            self.assertNotIn("src/old-source.tar.gz", names)

        with zipfile.ZipFile(output / f"spice2x-{version}-winxp.zip") as archive:
            self.assertEqual(
                set(archive.namelist()),
                {"spice.exe", "spicecfg.exe", "LICENSE.txt", "FORK_VERSION", "RELEASE.json", "README.txt"},
            )
            readme = archive.read("README.txt").decode("utf-8")
            self.assertIn("existing installation", readme)
            self.assertIn("back up/replace executables", readme)
            self.assertIn("preserve game files/modules/launch arguments", readme)
            self.assertIn("docs/Fork-setup.md", readme)

        with tarfile.open(output / f"spice2x-{version}-source.tar.gz", "r:gz") as archive:
            names = archive.getnames()
            self.assertTrue(any(name.endswith("/controller/app.js") for name in names))
            self.assertTrue(any(name.endswith("/src/spice2x/main.cpp") for name in names))
            self.assertTrue(any(name.endswith("/docs/guide.md") for name in names))
            self.assertTrue(any(name.endswith("/.github/workflows/ci.yml") for name in names))
            self.assertFalse(any("ignored.txt" in name for name in names))

        checksum_lines = (output / "SHA256SUMS").read_text(encoding="ascii").splitlines()
        self.assertEqual({line.split("  ", 1)[1] for line in checksum_lines}, expected - {"SHA256SUMS"})
        for line in checksum_lines:
            digest, name = line.split("  ", 1)
            actual = hashlib.sha256((output / name).read_bytes()).hexdigest()
            self.assertEqual(actual, digest)

    def test_rejects_missing_xp_binary(self):
        (self.binaries / "extras/winxp/spicecfg.exe").unlink()
        with self.assertRaisesRegex(PackageError, r"Windows XP binary.*spicecfg\.exe"):
            self._package()

    def test_rejects_dirty_tracked_source(self):
        self._write("controller/app.js", "changed\n")
        with self.assertRaisesRegex(PackageError, "tracked changes"):
            self._package()

    def test_rejects_untracked_source_and_stale_binary_build(self):
        self._write("controller/new.js", "uncommitted source\n")
        with self.assertRaisesRegex(PackageError, "untracked source"):
            self._package()
        (self.repo / "controller/new.js").unlink()
        self._git("archive", "--format=tar.gz", f"--output={self.build_source}", self.upstream_commit)
        with self.assertRaisesRegex(PackageError, "does not match"):
            self._package()
        self.build_source.unlink()
        with self.assertRaisesRegex(PackageError, "missing or invalid binary build source"):
            self._package()

    def test_rejects_nonempty_output_and_output_inside_binaries(self):
        output = self.work / "release"
        output.mkdir()
        (output / "sentinel").write_text("keep", encoding="utf-8")
        with self.assertRaisesRegex(PackageError, "not empty"):
            package_release(self.repo, self.binaries, output)
        with self.assertRaisesRegex(PackageError, "inside the binaries"):
            package_release(self.repo, self.binaries, self.binaries / "release")

    def test_staging_does_not_write_through_input_symlinks(self):
        sentinel = self.work / "sentinel"
        sentinel.write_text("keep", encoding="utf-8")
        (self.binaries / "FORK_VERSION").symlink_to(sentinel)
        with self.assertRaisesRegex(PackageError, "symlinks are not allowed"):
            self._package()
        self.assertEqual(sentinel.read_text(), "keep")
        output_link = self.work / "output-link"
        output_link.symlink_to(self.work / "release", target_is_directory=True)
        with self.assertRaisesRegex(PackageError, "symlink"):
            package_release(self.repo, self.binaries, output_link)

    def test_source_snapshot_stays_pinned_when_head_advances(self):
        copytree = shutil.copytree

        def advance_head(source, destination, *args, **kwargs):
            result = copytree(source, destination, *args, **kwargs)
            if Path(source) == self.binaries:
                self._write("controller/app.js", "new source\n")
                self._write("FORK_VERSION", "26-10-03-99\n")
                self._write("LICENSE", "new license\n")
                self._git("add", ".")
                self._git("commit", "-qm", "concurrent update fixture")
            return result

        with patch("package_release.shutil.copytree", side_effect=advance_head):
            output = self._package()
        metadata = json.loads((output / "RELEASE.json").read_text())
        self.assertNotEqual(self._git("rev-parse", "HEAD"), self.source_commit)
        self.assertEqual(metadata["source_commit"], self.source_commit)
        self.assertEqual(metadata["source_tree"], self.source_tree)
        version = "26-10-03-1"
        with tarfile.open(output / f"spice2x-{version}-source.tar.gz") as archive:
            self.assertEqual(archive.pax_headers["comment"], self.source_commit)
            source = archive.extractfile(f"spice2x-{version}/controller/app.js").read()
            self.assertEqual(source, b"console.log('controller');\n")
        with zipfile.ZipFile(output / f"spice2x-{version}.zip") as archive:
            self.assertEqual(archive.read("LICENSE.txt"), b"fixture license\n")
            self.assertEqual(archive.read("FORK_VERSION"), (version + "\n").encode())

    def test_rejects_invalid_version_or_upstream_base(self):
        self._write("FORK_VERSION", "not-a-version\n")
        self._git("add", "FORK_VERSION")
        self._git("commit", "-qm", "invalid version fixture")
        with self.assertRaisesRegex(PackageError, "FORK_VERSION"):
            self._package()

        self._write("FORK_VERSION", "26-10-03-1\n")
        self._write("UPSTREAM_BASE", "0" * 40 + "\n")
        self._git("add", "FORK_VERSION", "UPSTREAM_BASE")
        self._git("commit", "-qm", "invalid base fixture")
        with self.assertRaisesRegex(PackageError, "UPSTREAM_BASE"):
            self._package()

    def test_rejects_nonancestor_upstream_base(self):
        env = os.environ.copy()
        env.update(
            {
                "GIT_AUTHOR_NAME": "Other",
                "GIT_AUTHOR_EMAIL": "other@example.invalid",
                "GIT_COMMITTER_NAME": "Other",
                "GIT_COMMITTER_EMAIL": "other@example.invalid",
            }
        )
        unrelated = subprocess.run(
            ["git", "-C", str(self.repo), "commit-tree", f"{self.source_tree}^{{tree}}", "-m", "other"],
            check=True,
            capture_output=True,
            text=True,
            env=env,
        ).stdout.strip()
        self._write("UPSTREAM_BASE", unrelated + "\n")
        self._git("add", "UPSTREAM_BASE")
        self._git("commit", "-qm", "nonancestor fixture")
        with self.assertRaisesRegex(PackageError, "not an ancestor"):
            self._package()


if __name__ == "__main__":
    unittest.main()
