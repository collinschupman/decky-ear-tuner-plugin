import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SERVICE_SCRIPT = ROOT / "service.sh"


class SmartFilterServiceTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name).resolve()
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        self.service_script = self.root / "service.sh"
        self.service_script.write_text(
            SERVICE_SCRIPT.read_text(encoding="utf-8"), encoding="utf-8"
        )
        self.service_script.chmod(
            self.service_script.stat().st_mode
            | stat.S_IXUSR
            | stat.S_IXGRP
            | stat.S_IXOTH
        )
        self.backend = (
            self.root / "native" / "build-current" / "decky_ear_tuner_backend"
        )
        self.backend.parent.mkdir(parents=True)
        self._write_executable(
            self.backend,
            "#!/bin/bash\nprintf 'backend development\n' >> \"$FAKE_COMMAND_LOG\"\n",
        )
        self.release_backend = (
            self.root / "native" / "build" / "decky_ear_tuner_backend"
        )
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.command_log = self.root / "commands.log"
        self.config_home = self.root / "config"
        self.env = os.environ.copy()
        self.env.update(
            {
                "PATH": f"{self.bin}:{self.env['PATH']}",
                "XDG_RUNTIME_DIR": str(self.runtime),
                "XDG_CONFIG_HOME": str(self.config_home),
                "FLATPAK_ID": "",
                "FAKE_COMMAND_LOG": str(self.command_log),
            }
        )
        self._write_fake_commands()

    def tearDown(self):
        self.tempdir.cleanup()

    def _write_executable(self, path: Path, content: str):
        path.write_text(content, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def _write_fake_commands(self):
        self._write_executable(
            self.bin / "curl",
            "#!/bin/bash\n"
            'printf \'curl %s\\n\' "$*" >> "$FAKE_COMMAND_LOG"\n'
            'if [[ "${FAKE_CURL_STATUS:-0}" != 0 ]]; then\n'
            '    exit "$FAKE_CURL_STATUS"\n'
            "fi\n"
            'if [[ "$*" == *"/api/v1/diagnostics"* ]]; then\n'
            "    printf '%s\\n' '{\"ingress\":{\"deadline_misses\":0}}'\n"
            "    exit 0\n"
            "fi\n"
            "for _attempt in {1..100}; do\n"
            "    grep -q '^backend ' \"$FAKE_COMMAND_LOG\" && exit 0\n"
            "    /usr/bin/sleep 0.01\n"
            "done\n"
            "exit 1\n",
        )
        self._write_executable(
            self.bin / "pkill",
            '#!/bin/bash\nprintf \'pkill %s\\n\' "$*" >> "$FAKE_COMMAND_LOG"\nexit 0\n',
        )
        self._write_executable(
            self.bin / "pw-cli",
            "#!/bin/bash\n"
            'printf \'pw-cli %s\\n\' "$*" >> "$FAKE_COMMAND_LOG"\n'
            'exit "${FAKE_PW_CLI_STATUS:-0}"\n',
        )
        self._write_executable(
            self.bin / "pactl",
            "#!/bin/bash\n"
            'printf \'pactl %s\n\' "$*" >> "$FAKE_COMMAND_LOG"\n'
            "printf '%s\n' \"${FAKE_DEFAULT_SINK:-test-headphones}\"\n",
        )
        self._write_executable(
            self.bin / "flatpak-spawn",
            "#!/bin/bash\n"
            'printf \'flatpak-spawn %s\\n\' "$*" >> "$FAKE_COMMAND_LOG"\n'
            'if [[ "$1" == "--host" ]]; then\n'
            "    shift\n"
            '    export XDG_CONFIG_HOME="${HOST_XDG_CONFIG_HOME:-${XDG_CONFIG_HOME:-}}"\n'
            "fi\n"
            '"$@"\n',
        )
        self._write_executable(self.bin / "sleep", "#!/bin/bash\nexit 0\n")
        self._write_executable(
            self.bin / "systemctl",
            '#!/bin/bash\nprintf \'systemctl %s\\n\' "$*" >> "$FAKE_COMMAND_LOG"\n',
        )
        self._write_executable(
            self.bin / "ps",
            "#!/bin/bash\n"
            'printf \'ps %s\\n\' "$*" >> "$FAKE_COMMAND_LOG"\n'
            "printf '%s\\n' '1234  FF     88 128 data-loop'\n",
        )

    def _run_service(self, command: str, **environment):
        env = self.env.copy()
        env.update(environment)
        return subprocess.run(
            ["bash", str(self.service_script), command],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )

    def _commands(self) -> str:
        if not self.command_log.exists():
            return ""
        return self.command_log.read_text(encoding="utf-8")

    def test_start_launches_backend_and_waits_for_control_api(self):
        completed = self._run_service("start")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("Ear Tuner started", completed.stdout)
        self.assertTrue((self.runtime / "ear-tuner-backend.pid").exists())
        self.assertIn(
            "curl -sf http://127.0.0.1:19285/api/v1/control?left=0&right=0",
            self._commands(),
        )
        self.assertIn("backend development", self._commands())

    def test_start_leaves_output_selection_to_wireplumber(self):
        completed = self._run_service("start")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertNotIn("pactl", self._commands())
        self.assertIn("backend development", self._commands())

    def test_start_falls_back_to_release_backend(self):
        self.backend.unlink()
        self.release_backend.parent.mkdir(parents=True)
        self._write_executable(
            self.release_backend,
            "#!/bin/bash\nprintf 'backend release\\n' >> \"$FAKE_COMMAND_LOG\"\n",
        )

        completed = self._run_service("start")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("backend release", self._commands())

    def test_start_fails_when_no_backend_is_installed(self):
        self.backend.unlink()

        completed = self._run_service("start")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("not found", completed.stderr)
        self.assertFalse((self.runtime / "ear-tuner-backend.pid").exists())

    def test_start_does_not_mutate_audio_routes(self):
        completed = self._run_service("start")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        commands = self._commands()
        self.assertNotIn("wpctl", commands)
        self.assertNotIn("pw-link", commands)
        self.assertNotIn("systemctl", commands)

    def test_start_restarts_an_existing_backend(self):
        completed = self._run_service("start")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn(f"pkill -TERM -f ^{self.backend}( |$)", self._commands())

    def test_start_failure_removes_pid_file_and_stops_backend(self):
        completed = self._run_service("start", FAKE_CURL_STATUS="1")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("Backend not ready", completed.stderr)
        self.assertFalse((self.runtime / "ear-tuner-backend.pid").exists())
        self.assertIn(f"pkill -TERM -f ^{self.backend}( |$)", self._commands())

    def test_stop_terminates_backend_and_removes_pid_file(self):
        pid_file = self.runtime / "ear-tuner-backend.pid"
        pid_file.write_text("999999\n", encoding="utf-8")

        completed = self._run_service("stop")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertFalse(pid_file.exists())
        commands = self._commands()
        self.assertIn(f"pkill -TERM -f ^{self.backend}( |$)", commands)
        self.assertIn(f"pkill -KILL -f ^{self.backend}( |$)", commands)

    def test_uninstall_uses_the_stop_lifecycle(self):
        policy_file = (
            self.config_home
            / "pipewire"
            / "pipewire-pulse.conf.d"
            / "90-ear-tuner-alert-routing.conf"
        )
        policy_file.parent.mkdir(parents=True)
        policy_file.write_text("legacy policy\n", encoding="utf-8")

        completed = self._run_service("uninstall")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("Ear Tuner stopped", completed.stdout)
        self.assertIn(f"pkill -TERM -f ^{self.backend}( |$)", self._commands())
        self.assertFalse(policy_file.exists())
        self.assertIn(
            "systemctl --user restart pipewire-pulse.service", self._commands()
        )

    def test_status_reports_stopped_without_pid_file(self):
        completed = self._run_service("status")

        self.assertEqual(completed.returncode, 0)
        self.assertIn("backend  : stopped", completed.stdout)

    def test_status_reports_stale_pid_file(self):
        (self.runtime / "ear-tuner-backend.pid").write_text(
            "999999\n", encoding="utf-8"
        )

        completed = self._run_service("status")

        self.assertEqual(completed.returncode, 0)
        self.assertIn("stale pid file", completed.stdout)

    def test_status_reports_scheduler_and_audio_diagnostics(self):
        (self.runtime / "ear-tuner-backend.pid").write_text(
            f"{os.getpid()}\n", encoding="utf-8"
        )

        completed = self._run_service("status")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("FF     88", completed.stdout)
        self.assertIn('"deadline_misses":0', completed.stdout)
        self.assertIn(f"ps -L -p {os.getpid()}", self._commands())
        self.assertIn("/api/v1/diagnostics", self._commands())

    def test_output_ready_checks_pipewire_core(self):
        completed = self._run_service("output-ready")

        self.assertEqual(completed.returncode, 0)
        self.assertIn("pw-cli info 0", self._commands())

    def test_output_id_reports_the_current_default_sink(self):
        completed = self._run_service("output-id", FAKE_DEFAULT_SINK="usb-headset")

        self.assertEqual(completed.returncode, 0)
        self.assertEqual(completed.stdout.strip(), "usb-headset")
        self.assertIn("pactl get-default-sink", self._commands())

    def test_output_ready_propagates_pipewire_failure(self):
        completed = self._run_service("output-ready", FAKE_PW_CLI_STATUS="1")

        self.assertNotEqual(completed.returncode, 0)

    def test_flatpak_commands_execute_on_host(self):
        completed = self._run_service(
            "output-ready", FLATPAK_ID="com.valvesoftware.Steam"
        )

        self.assertEqual(completed.returncode, 0)
        self.assertIn("flatpak-spawn --host pw-cli info 0", self._commands())

    def test_flatpak_stop_manages_pid_file_on_host(self):
        pid_file = self.runtime / "ear-tuner-backend.pid"
        pid_file.write_text("999999\n", encoding="utf-8")

        completed = self._run_service("stop", FLATPAK_ID="com.visualstudio.code")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        commands = self._commands()
        self.assertIn(f"flatpak-spawn --host cat {pid_file}", commands)
        self.assertIn(f"flatpak-spawn --host rm -f {pid_file}", commands)
        self.assertFalse(pid_file.exists())

    def test_unknown_command_returns_usage_error(self):
        completed = self._run_service("watch-route")

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("Usage:", completed.stderr)


if __name__ == "__main__":
    unittest.main()
