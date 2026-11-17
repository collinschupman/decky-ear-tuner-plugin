import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from ear_tuner.windows_desktop import _instance_lock, main


class WindowsDesktopTests(unittest.TestCase):
    def test_instance_lock_initializes_byte_and_unlocks_on_failure(self):
        windows_lock = Mock(LK_NBLCK=1, LK_UNLCK=2)
        with (
            tempfile.TemporaryDirectory() as temporary,
            patch.dict(sys.modules, {"msvcrt": windows_lock}),
        ):
            lock_path = Path(temporary) / "desktop.lock"
            with self.assertRaisesRegex(RuntimeError, "startup failure"):
                with _instance_lock(lock_path):
                    self.assertEqual(lock_path.read_bytes(), b"\0")
                    raise RuntimeError("startup failure")
            self.assertEqual(
                [call.args[1:] for call in windows_lock.locking.call_args_list],
                [(1, 1), (2, 1)],
            )
            with _instance_lock(lock_path):
                self.assertEqual(lock_path.stat().st_size, 1)

    def test_lock_failure_does_not_run_desktop_or_unlock_someone_else(self):
        windows_lock = Mock(LK_NBLCK=1, LK_UNLCK=2)
        windows_lock.locking.side_effect = PermissionError("locked")
        with (
            tempfile.TemporaryDirectory() as temporary,
            patch.dict(sys.modules, {"msvcrt": windows_lock}),
        ):
            with self.assertRaises(PermissionError):
                with _instance_lock(Path(temporary) / "desktop.lock"):
                    self.fail("An already locked instance must not start")
            windows_lock.locking.assert_called_once()

    @unittest.skipUnless(
        sys.platform == "win32", "Requires real Windows byte-range locks"
    )
    def test_real_windows_lock_excludes_another_process_and_is_reusable(self):
        code = (
            "from pathlib import Path; import sys; "
            "from ear_tuner.windows_desktop import _instance_lock; "
            "\nwith _instance_lock(Path(sys.argv[1])): print('acquired')"
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "desktop.lock"
            arguments = [sys.executable, "-c", code, str(path)]
            with _instance_lock(path):
                denied = subprocess.run(arguments, capture_output=True, timeout=5)
                self.assertNotEqual(denied.returncode, 0)
                self.assertIn(b"PermissionError", denied.stderr)
            allowed = subprocess.run(arguments, capture_output=True, timeout=5)
            self.assertEqual(allowed.returncode, 0, allowed.stderr)
            self.assertEqual(allowed.stdout.strip(), b"acquired")

    def test_launcher_rejects_non_windows_without_starting_native_process(self):
        with (
            patch("sys.platform", "darwin"),
            patch("sys.argv", ["ear-tuner-windows"]),
            patch("sys.stderr"),
            patch("subprocess.run") as run,
        ):
            with self.assertRaises(SystemExit) as result:
                main()
            self.assertEqual(result.exception.code, 2)
            run.assert_not_called()

    def test_launcher_lists_devices_without_loading_settings_or_locking(self):
        with (
            patch("sys.platform", "win32"),
            patch("sys.argv", ["ear-tuner-windows", "--list-devices"]),
            patch.object(Path, "is_file", return_value=True),
            patch("subprocess.run", return_value=Mock(returncode=0)) as run,
            patch("ear_tuner.windows_desktop._instance_lock") as lock,
        ):
            self.assertEqual(main(), 0)
            self.assertEqual(run.call_args.args[0][-1], "--list-devices")
            self.assertTrue(
                str(run.call_args.args[0][0]).endswith("ear_tuner_windows.exe")
            )
            lock.assert_not_called()

    def test_launcher_rejects_missing_endpoint_selection_and_invalid_ports(self):
        for arguments in (
            [],
            ["--input", "cable"],
            ["--port", "0"],
            ["--port", "65536"],
        ):
            with (
                self.subTest(arguments=arguments),
                patch("sys.platform", "win32"),
                patch("sys.argv", ["ear-tuner-windows", *arguments]),
                patch.object(Path, "is_file", return_value=True),
                patch("sys.stderr"),
                patch.dict(os.environ, {"LOCALAPPDATA": "unused"}),
            ):
                with self.assertRaises(SystemExit) as result:
                    main()
                self.assertEqual(result.exception.code, 2)
