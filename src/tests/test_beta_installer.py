import os
import stat
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INSTALLER = ROOT / "scripts" / "install_beta.sh"


class BetaInstallerTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.decky_home = self.root / "homebrew"
        loader = self.decky_home / "services" / "PluginLoader"
        loader.parent.mkdir(parents=True)
        loader.touch()
        loader.chmod(loader.stat().st_mode | stat.S_IXUSR)

    def tearDown(self):
        self.tempdir.cleanup()

    def create_release_archive(self):
        package_root = self.root / "package"
        (package_root / "dist").mkdir(parents=True)
        native = package_root / "native" / "build"
        native.mkdir(parents=True)
        application_package = package_root / "ear_tuner"
        application_package.mkdir()
        (package_root / "package.json").write_text(
            '{"type": "module"}', encoding="utf-8"
        )
        (package_root / "plugin.json").write_text("{}", encoding="utf-8")
        (package_root / "main.py").write_text("", encoding="utf-8")
        (package_root / "service.sh").write_text("#!/bin/sh\n", encoding="utf-8")
        (package_root / "dist" / "index.js").write_text("", encoding="utf-8")
        (application_package / "application_service.py").write_text(
            "", encoding="utf-8"
        )
        native_backend = native / "decky_ear_tuner_backend"
        native_backend.write_text("#!/bin/sh\n", encoding="utf-8")
        native_backend.chmod(native_backend.stat().st_mode | stat.S_IXUSR)

        archive = self.root / "ear-tuner.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            for file_path in package_root.rglob("*"):
                if file_path.is_file():
                    tar.add(file_path, file_path.relative_to(package_root))
        return archive

    def test_installs_release_and_preserves_settings(self):
        archive = self.create_release_archive()
        plugin_dir = self.decky_home / "plugins" / "ear-tuner"
        plugin_dir.mkdir(parents=True)
        old_service = plugin_dir / "service.sh"
        old_service.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        old_service.chmod(old_service.stat().st_mode | stat.S_IXUSR)
        settings = self.decky_home / "settings" / "ear-tuner" / "settings.json"
        settings.parent.mkdir(parents=True)
        settings.write_text('{"enabled": true}', encoding="utf-8")
        command_log = self.root / "tar-commands.log"
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        tar_wrapper = fake_bin / "tar"
        tar_wrapper.write_text(
            '#!/bin/sh\nprintf \'%s\\n\' "$*" >> "$TAR_COMMAND_LOG"\n'
            'exec /usr/bin/tar "$@"\n',
            encoding="utf-8",
        )
        tar_wrapper.chmod(tar_wrapper.stat().st_mode | stat.S_IXUSR)

        completed = subprocess.run(
            ["bash", str(INSTALLER), str(archive)],
            capture_output=True,
            text=True,
            env={
                **os.environ,
                "DECKY_HOME": str(self.decky_home),
                "PATH": f"{fake_bin}:{os.environ['PATH']}",
                "TAR_COMMAND_LOG": str(command_log),
            },
            check=False,
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            (plugin_dir / "package.json").read_text(encoding="utf-8"),
            '{"type": "module"}',
        )
        self.assertTrue((plugin_dir / "dist" / "index.js").is_file())
        self.assertTrue(
            (plugin_dir / "native" / "build" / "decky_ear_tuner_backend").is_file()
        )
        self.assertEqual(settings.read_text(encoding="utf-8"), '{"enabled": true}')
        extraction_command = next(
            command
            for command in command_log.read_text(encoding="utf-8").splitlines()
            if command.startswith("-xzf ")
        )
        self.assertIn(f"-C {self.decky_home}/.ear-tuner.stage.", extraction_command)
        self.assertNotIn(str(self.decky_home / "plugins"), extraction_command)


if __name__ == "__main__":
    unittest.main()
