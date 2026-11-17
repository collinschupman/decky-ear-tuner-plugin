import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PackagingTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "Node.js is required for build staging")
    def test_staged_runtime_imports_without_source_and_excludes_development_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "scripts" / "stage_runtime.mjs"
            script.parent.mkdir()
            shutil.copyfile(ROOT / "scripts" / "stage_runtime.mjs", script)
            source = root / "src" / "ear_tuner"
            shutil.copytree(ROOT / "src" / "ear_tuner", source)
            (source / "control.token").write_text("must not ship")
            destination = root / "ear_tuner"
            destination.mkdir()
            (destination / "obsolete.py").touch()
            shutil.copyfile(ROOT / "main.py", root / "main.py")

            completed = subprocess.run(
                ["node", str(script)], capture_output=True, text=True, check=False
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(
                {path.name for path in destination.iterdir()},
                {path.name for path in source.glob("*.py")},
            )
            (root / "src").rename(root / "excluded-src")
            imported = subprocess.run(
                [
                    sys.executable,
                    "-I",
                    "-c",
                    "import sys; sys.path.insert(0, '.'); import main; "
                    "from ear_tuner.control_credentials import control_request",
                ],
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(imported.returncode, 0, imported.stderr)

    def test_packaging_requires_backend_target_even_when_a_stale_binary_exists(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "scripts" / "package_decky.sh"
            script.parent.mkdir()
            shutil.copyfile(ROOT / "scripts" / "package_decky.sh", script)
            stale_binary = root / "native" / "build" / "decky_ear_tuner_backend"
            stale_binary.parent.mkdir(parents=True)
            stale_binary.write_text("stale executable", encoding="utf-8")
            stale_binary.chmod(0o755)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            license_dir = root / "decky"
            license_dir.mkdir()
            (license_dir / "LICENSE").write_text("test license", encoding="utf-8")
            commands = {
                "node": (
                    "#!/bin/sh\n"
                    'case "$*" in\n'
                    "  *version*) echo 0.1.0 ;;\n"
                    f'  *) echo "{license_dir}/package.json" ;;\n'
                    "esac\n"
                ),
                "pnpm": "#!/bin/sh\nexit 0\n",
                "cmake": (
                    "#!/bin/sh\n"
                    'printf "%s\\n" "$*" >> "$COMMAND_LOG"\n'
                    'case "$*" in\n'
                    '  *"--target decky_ear_tuner_backend"*)\n'
                    '    echo "No rule to make target decky_ear_tuner_backend" >&2\n'
                    "    exit 2 ;;\n"
                    "esac\n"
                    "exit 0\n"
                ),
                "strip": '#!/bin/sh\necho strip >> "$COMMAND_LOG"\n',
            }
            for name, content in commands.items():
                executable = bin_dir / name
                executable.write_text(content, encoding="utf-8")
                executable.chmod(0o755)
            command_log = root / "commands.log"
            completed = subprocess.run(
                ["bash", str(script)],
                env={
                    **os.environ,
                    "PATH": f"{bin_dir}:{os.environ['PATH']}",
                    "COMMAND_LOG": str(command_log),
                },
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("No rule to make target", completed.stderr)
            self.assertIn("--target decky_ear_tuner_backend", command_log.read_text())
            self.assertNotIn("strip", command_log.read_text())
            self.assertFalse((root / "release").exists())


if __name__ == "__main__":
    unittest.main()
