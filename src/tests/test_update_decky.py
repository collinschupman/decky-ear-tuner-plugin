import os
import stat
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UPDATE_SCRIPT = ROOT / "update-decky.sh"


class UpdateDeckyScriptTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        (self.root / ".git").mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "commands.log"
        self.update_script = self.root / "update-decky.sh"
        self.update_script.write_bytes(UPDATE_SCRIPT.read_bytes())
        self.update_script.chmod(self.update_script.stat().st_mode | stat.S_IXUSR)
        self._write_executable(
            self.root / "install-decky.sh",
            '#!/usr/bin/env bash\nprintf "install %s\\n" "$*" >> "${FAKE_LOG}"\n',
        )
        self._write_executable(
            self.bin / "git",
            textwrap.dedent(
                """\
                #!/usr/bin/env bash
                case "$1" in
                  status)
                    [[ -n "${FAKE_GIT_DIRTY:-}" ]] && echo " M local-change"
                    ;;
                  rev-parse)
                    echo "origin/master"
                    ;;
                  pull)
                    printf 'git %s\n' "$*" >> "${FAKE_LOG}"
                    ;;
                  *)
                    exit 1
                    ;;
                esac
                """
            ),
        )

    def tearDown(self):
        self.tempdir.cleanup()

    @staticmethod
    def _write_executable(path: Path, content: str):
        path.write_text(content, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)

    def _run_update(self, *args: str, dirty: bool = False):
        env = os.environ.copy()
        env["PATH"] = f"{self.bin}:{env['PATH']}"
        env["FAKE_LOG"] = str(self.log)
        if dirty:
            env["FAKE_GIT_DIRTY"] = "1"
        return subprocess.run(
            ["bash", str(self.update_script), *args],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )

    def test_clean_checkout_pulls_and_runs_installer(self):
        completed = self._run_update("--skip-system-deps")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            self.log.read_text(encoding="utf-8").splitlines(),
            ["git pull --ff-only", "install --skip-system-deps"],
        )

    def test_dirty_checkout_is_not_updated(self):
        completed = self._run_update(dirty=True)

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("local changes", completed.stderr)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
