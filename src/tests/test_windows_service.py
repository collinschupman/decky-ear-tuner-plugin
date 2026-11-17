import asyncio
import http.client
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, Mock, patch

from ear_tuner import windows_service
from ear_tuner.settings import DEFAULT_EAR_TUNER, SettingsManager
from ear_tuner.windows_desktop import WindowsDesktopPlugin
from ear_tuner.windows_service import WindowsAudioService, WindowsBackendClient

NATIVE_HELPER = r"""
import argparse
import json
import os
import signal
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

parser = argparse.ArgumentParser()
parser.add_argument("--port", type=int)
parser.add_argument("--port-file")
parser.add_argument("--mode-file")
parser.add_argument("--record-file")
parser.add_argument("--session")
parser.add_argument("--input")
parser.add_argument("--output")
parser.add_argument("--ignore-term", action="store_true")
args = parser.parse_args()
if args.ignore_term:
    signal.signal(signal.SIGTERM, signal.SIG_IGN)

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        mode = Path(args.mode_file).read_text()
        if self.path.startswith("/api/v1/diagnostics"):
            body = json.dumps({
                "platform": "wasapi",
                "session": "foreign-session" if mode == "foreign" else args.session,
                "pid": os.getpid(),
                "audio_ready": mode != "not-ready",
            }).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body) + (10 if mode == "truncated" else 0)))
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
        elif self.path.startswith("/api/v1/control"):
            record = Path(args.record_file)
            temporary = record.with_suffix(".tmp")
            temporary.write_text(json.dumps(parse_qs(urlparse(self.path).query)))
            temporary.replace(record)
            if mode == "bad-control":
                self.wfile.write(b"NOT HTTP\r\n\r\n")
                self.close_connection = True
                return
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"OK")
        else:
            self.send_error(404)

HTTPServer.allow_reuse_address = True
server = HTTPServer(("127.0.0.1", args.port), Handler)
port = Path(args.port_file)
temporary = port.with_suffix(".tmp")
temporary.write_text(str(server.server_address[1]))
temporary.replace(port)
server.serve_forever()
"""


class WindowsServiceTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.logger = Mock()
        self.service = WindowsAudioService(
            Path("/native/ear_tuner_windows"),
            "cable-capture-uid",
            "headphones-uid",
            Path(self.temporary.name) / "native.log",
            self.logger,
        )
        self.process = Mock(pid=1234, returncode=None)
        self.process.wait = AsyncMock(return_value=0)

    async def test_readiness_requires_owned_process_and_audio_callbacks(self):
        self.service._process = self.process
        self.service._session = "our-session"
        diagnostics = {
            "platform": "wasapi",
            "session": "our-session",
            "pid": 1234,
            "audio_ready": True,
        }
        with patch.object(self.service, "_read_diagnostics", return_value=diagnostics):
            self.assertTrue(await self.service.output_ready())
            for key, bad in (
                ("session", "another-session"),
                ("pid", 999),
                ("audio_ready", False),
                ("platform", "pipewire"),
            ):
                previous = diagnostics[key]
                diagnostics[key] = bad
                self.assertFalse(await self.service.output_ready())
                diagnostics[key] = previous
            self.process.returncode = 1
            self.assertFalse(await self.service.output_ready())

    async def test_start_passes_explicit_device_uids_and_waits_for_readiness(self):
        with (
            patch(
                "ear_tuner.windows_service.asyncio.create_subprocess_exec",
                AsyncMock(return_value=self.process),
            ) as create,
            patch.object(
                self.service, "output_ready", AsyncMock(side_effect=[False, True])
            ),
        ):
            self.assertTrue(await self.service.execute("start"))
        args = create.call_args.args
        self.assertEqual(
            args[:5],
            (
                str(self.service.executable),
                "--input",
                "cable-capture-uid",
                "--output",
                "headphones-uid",
            ),
        )
        self.assertEqual(args[5:], ("--session", self.service._session))
        await self.service.execute("stop")
        self.process.terminate.assert_called_once()
        self.process.wait.assert_awaited_once()

    async def test_start_failure_is_reported_and_child_is_reaped(self):
        self.process.returncode = 1
        with patch(
            "ear_tuner.windows_service.asyncio.create_subprocess_exec",
            AsyncMock(return_value=self.process),
        ):
            self.assertFalse(await self.service.execute("start"))
        self.assertIsNone(self.service._process)
        self.logger.error.assert_called_once()

    async def test_cancelling_start_terminates_only_the_owned_child(self):
        entered = asyncio.Event()

        async def ready():
            if self.service._process is not None:
                entered.set()
                await asyncio.Future()
            return False

        with (
            patch(
                "ear_tuner.windows_service.asyncio.create_subprocess_exec",
                AsyncMock(return_value=self.process),
            ),
            patch.object(self.service, "output_ready", ready),
        ):
            task = asyncio.create_task(self.service.execute("start"))
            await asyncio.wait_for(entered.wait(), 1)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
        self.process.terminate.assert_called_once()
        self.process.wait.assert_awaited_once()

    async def test_unresponsive_owned_child_is_killed_after_timeout(self):
        self.service._process = self.process
        self.process.wait.side_effect = [asyncio.TimeoutError(), 0]
        self.assertTrue(await self.service.execute("stop"))
        self.process.terminate.assert_called_once()
        self.process.kill.assert_called_once()
        self.assertIsNone(self.service._process)
        self.logger.warning.assert_called_once()

    async def test_cancellation_during_spawn_still_reaps_the_child(self):
        entered = asyncio.Event()
        finish = asyncio.Event()

        async def create(*_args, **_kwargs):
            entered.set()
            await finish.wait()
            return self.process

        with patch("ear_tuner.windows_service.asyncio.create_subprocess_exec", create):
            task = asyncio.create_task(self.service.execute("start"))
            await asyncio.wait_for(entered.wait(), 1)
            task.cancel()
            await asyncio.sleep(0)
            self.assertFalse(task.done())
            finish.set()
            with self.assertRaises(asyncio.CancelledError):
                await asyncio.wait_for(task, 1)
        self.process.terminate.assert_called_once()
        self.process.wait.assert_awaited_once()

    async def test_shutdown_serializes_with_inflight_startup(self):
        entered = asyncio.Event()
        finish = asyncio.Event()

        async def ready():
            if self.service._process is None:
                return False
            entered.set()
            await finish.wait()
            return True

        with (
            patch(
                "ear_tuner.windows_service.asyncio.create_subprocess_exec",
                AsyncMock(return_value=self.process),
            ),
            patch.object(self.service, "output_ready", ready),
        ):
            start = asyncio.create_task(self.service.execute("start"))
            await asyncio.wait_for(entered.wait(), 1)
            stop = asyncio.create_task(self.service.execute("stop"))
            await asyncio.sleep(0)
            self.assertFalse(stop.done())
            finish.set()
            self.assertEqual(await asyncio.gather(start, stop), [True, True])
        self.process.terminate.assert_called_once()

    async def test_control_client_does_not_update_an_unowned_backend(self):
        client = WindowsBackendClient(self.service, self.logger)
        with patch.object(client, "_request", AsyncMock()) as request:
            self.assertFalse(await client.is_healthy())
            self.assertFalse(await client.apply_ear_tuner(DEFAULT_EAR_TUNER))
        request.assert_not_awaited()
        self.logger.warning.assert_called_once()

    async def test_truncated_diagnostics_are_not_reported_as_ready(self):
        self.service._process = self.process
        with patch.object(
            self.service,
            "_read_diagnostics",
            side_effect=http.client.IncompleteRead(b'{"audio_ready":'),
        ):
            self.assertFalse(await self.service.output_ready())

    async def test_desktop_start_failure_preserves_settings(self):
        settings = Mock()
        plugin = WindowsDesktopPlugin(settings, self.service, self.logger)
        with patch.object(self.service, "execute", AsyncMock(return_value=False)):
            with self.assertRaisesRegex(RuntimeError, "Audio startup failed"):
                await plugin.restore()
        settings.setSetting.assert_not_called()

    async def test_desktop_stops_monitor_and_native_process(self):
        plugin = WindowsDesktopPlugin(Mock(), self.service, self.logger)
        with (
            patch.object(self.service, "execute", AsyncMock(return_value=True)),
            patch.object(self.service, "output_ready", AsyncMock(return_value=True)),
            patch.object(plugin._coordinator, "restore", AsyncMock()),
            patch.object(
                plugin._coordinator, "stop", AsyncMock(return_value=True)
            ) as stop,
        ):
            await plugin.restore()
            self.assertIsNotNone(plugin._monitor)
            self.assertTrue(await plugin.stop())
        self.assertTrue(plugin._monitor.cancelled())
        stop.assert_awaited_once()

    async def test_desktop_reports_audio_failure_during_settings_restore(self):
        plugin = WindowsDesktopPlugin(Mock(), self.service, self.logger)
        with (
            patch.object(self.service, "execute", AsyncMock(return_value=True)),
            patch.object(plugin._coordinator, "restore", AsyncMock()),
            patch.object(self.service, "output_ready", AsyncMock(return_value=False)),
        ):
            with self.assertRaisesRegex(RuntimeError, "Settings restore failed"):
                await plugin.restore()
        self.assertIsNone(plugin._monitor)


class WindowsProcessIntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.mode = self.root / "mode"
        self.mode.write_text("normal")
        self.record = self.root / "controls.json"
        self.logger = Mock()
        self.service = WindowsAudioService(
            Path("/test/native"),
            "cable-capture-uid",
            "headphones-uid",
            self.root / "native.log",
            self.logger,
        )
        self.port = 0
        self.children = []
        self.ignore_term = False
        self.real_create = asyncio.create_subprocess_exec
        self.spawn_patch = patch(
            "ear_tuner.windows_service.asyncio.create_subprocess_exec",
            side_effect=self._launch_helper,
        )
        self.spawn_patch.start()
        self.addCleanup(self.spawn_patch.stop)
        for name in ("DIAGNOSTICS_URL", "CONTROL_URL"):
            patcher = patch.object(
                windows_service, name, getattr(windows_service, name)
            )
            patcher.start()
            self.addCleanup(patcher.stop)

    async def asyncTearDown(self):
        try:
            await asyncio.wait_for(self.service.execute("stop"), 8)
        finally:
            for child in self.children:
                if child.returncode is None:
                    child.kill()
                await asyncio.wait_for(child.wait(), 3)
            self.temporary.cleanup()

    async def _launch_helper(self, _executable, *arguments, **keywords):
        port_file = self.root / f"port-{len(self.children)}"
        options = ["--ignore-term"] if self.ignore_term else []
        process = await self.real_create(
            sys.executable,
            "-c",
            NATIVE_HELPER,
            "--port",
            str(self.port),
            "--port-file",
            str(port_file),
            "--mode-file",
            str(self.mode),
            "--record-file",
            str(self.record),
            *options,
            *arguments,
            **keywords,
        )
        self.children.append(process)
        deadline = asyncio.get_running_loop().time() + 3
        while not port_file.exists():
            if process.returncode is not None:
                self.fail((self.root / "native.log").read_text())
            if asyncio.get_running_loop().time() > deadline:
                self.fail("Native helper did not publish its local port")
            await asyncio.sleep(0.01)
        self.port = int(port_file.read_text())
        windows_service.DIAGNOSTICS_URL = (
            f"http://127.0.0.1:{self.port}/api/v1/diagnostics"
        )
        windows_service.CONTROL_URL = f"http://127.0.0.1:{self.port}/api/v1/control"
        return process

    async def test_real_child_is_shared_by_concurrent_starts_and_reaped_on_stop(self):
        results = await asyncio.wait_for(
            asyncio.gather(*(self.service.execute("start") for _ in range(8))), 5
        )
        self.assertEqual(results, [True] * 8)
        self.assertEqual(len(self.children), 1)
        client = WindowsBackendClient(self.service, self.logger)
        settings = DEFAULT_EAR_TUNER | {
            "enabled": True,
            "separate_channels": True,
            "left_mode": 1,
            "right_mode": 3,
        }
        self.assertTrue(await client.apply_ear_tuner(settings))
        sent = json.loads(self.record.read_text())
        self.assertEqual(sent["focus_enabled"], ["1"])
        self.assertEqual(sent["left_mode"], ["1"])
        self.assertEqual(sent["right_mode"], ["3"])
        self.assertEqual(
            await asyncio.gather(*(self.service.execute("stop") for _ in range(8))),
            [True] * 8,
        )
        self.assertIsNotNone(self.children[0].returncode)
        self.assertFalse(await client.is_healthy())

    async def test_real_truncated_and_foreign_diagnostics_block_control_updates(self):
        self.assertTrue(await self.service.execute("start"))
        client = WindowsBackendClient(self.service, self.logger)
        for mode in ("truncated", "foreign", "not-ready"):
            self.mode.write_text(mode)
            self.assertFalse(await self.service.output_ready())
            self.assertFalse(await client.apply_ear_tuner(DEFAULT_EAR_TUNER))
            self.assertFalse(self.record.exists())

    async def test_real_child_that_never_becomes_ready_is_stopped_at_deadline(self):
        self.mode.write_text("not-ready")
        self.assertFalse(
            await asyncio.wait_for(self.service.execute("start"), timeout=15)
        )
        self.assertEqual(len(self.children), 1)
        self.assertIsNotNone(self.children[0].returncode)
        self.assertIsNone(self.service._process)
        self.logger.error.assert_called_once()

    @unittest.skipIf(sys.platform == "win32", "TerminateProcess cannot be ignored")
    async def test_real_child_ignoring_termination_is_killed_and_reaped(self):
        self.ignore_term = True
        self.assertTrue(await self.service.execute("start"))
        self.assertTrue(await asyncio.wait_for(self.service.execute("stop"), 8))
        self.assertEqual(self.children[0].returncode, -9)
        self.logger.warning.assert_called_once()

    async def test_monitor_recovers_real_child_and_reapplies_saved_settings(self):
        self.assertTrue(await self.service.execute("start"))
        settings = SettingsManager("settings", str(self.root))
        settings.setSetting("ear_tuner", DEFAULT_EAR_TUNER | {"enabled": True})
        plugin = WindowsDesktopPlugin(settings, self.service, self.logger)
        try:
            await plugin.restore()
            self.record.unlink()
            self.children[0].kill()
            await self.children[0].wait()
            deadline = asyncio.get_running_loop().time() + 8
            while len(self.children) < 2 or not await self.service.output_ready():
                if asyncio.get_running_loop().time() > deadline:
                    self.fail("Health monitor did not restart the owned native child")
                await asyncio.sleep(0.05)
            while not self.record.exists() or json.loads(self.record.read_text()).get(
                "focus_enabled"
            ) != ["1"]:
                if asyncio.get_running_loop().time() > deadline:
                    self.fail("Saved controls were not reapplied after restart")
                await asyncio.sleep(0.05)
            self.assertTrue(settings.getSetting("ear_tuner")["enabled"])
        finally:
            self.assertTrue(await plugin.stop())
        self.assertTrue(all(child.returncode is not None for child in self.children))

    async def test_malformed_control_response_is_an_explicit_failure(self):
        self.assertTrue(await self.service.execute("start"))
        self.mode.write_text("bad-control")
        client = WindowsBackendClient(self.service, self.logger)
        self.assertFalse(await client.apply_ear_tuner(DEFAULT_EAR_TUNER))
        self.assertFalse(await client.is_healthy())
        self.logger.warning.assert_called_once()


if __name__ == "__main__":
    unittest.main()
