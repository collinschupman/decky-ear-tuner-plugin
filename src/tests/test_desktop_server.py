import asyncio
import http.client
import io
import json
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from ear_tuner.desktop_server import run_desktop_server


class FakePlugin:
    def __init__(self):
        self.ear_tuner = {"enabled": False, "left_mode": 2}
        self.applied_values = []
        self.apply_result = True
        self.restored = False
        self.stopped = False
        self.loops = []

    async def restore(self):
        self.loops.append(asyncio.get_running_loop())
        self.restored = True

    async def get_ear_tuner(self):
        self.loops.append(asyncio.get_running_loop())
        return self.ear_tuner

    async def get_service_status(self):
        self.loops.append(asyncio.get_running_loop())
        return {"backend_running": False, "output_ready": False}

    async def set_ear_tuner(self, value):
        self.loops.append(asyncio.get_running_loop())
        self.applied_values.append(value)
        return self.apply_result

    async def stop(self):
        self.loops.append(asyncio.get_running_loop())
        self.stopped = True


class DesktopServerTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.web_root = self.root / "dist-web"
        self.web_root.mkdir()
        (self.web_root / "index.html").write_text("<h1>UI</h1>", encoding="utf-8")
        self.plugin = FakePlugin()
        self.ready = threading.Event()
        self.server_holder = []
        self.thread = None

    def tearDown(self):
        if self.server_holder:
            self.server_holder[0].shutdown()
        if self.thread:
            self.thread.join(timeout=3)
        self.tempdir.cleanup()

    def _start_server(self):
        real_server = __import__("http.server").server.ThreadingHTTPServer

        def create_server(address, handler):
            server = real_server((address[0], 0), handler)
            self.server_holder.append(server)
            self.ready.set()
            return server

        self.thread = threading.Thread(
            target=run_desktop_server,
            args=(str(self.root), lambda: self.plugin, "127.0.0.1", 0),
            daemon=True,
        )
        with patch(
            "ear_tuner.desktop_server.ThreadingHTTPServer", side_effect=create_server
        ):
            self.thread.start()
            self.assertTrue(self.ready.wait(timeout=3))
        self.host, self.port = self.server_holder[0].server_address

    def _request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=2)
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        result = response.status, response.read(), dict(response.getheaders())
        connection.close()
        return result

    def test_routes_static_files_gets_posts_and_validation(self):
        self._start_server()
        self.assertTrue(self.plugin.restored)

        status, body, headers = self._request("GET", "/")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"<h1>UI</h1>")
        self.assertEqual(headers["Cache-Control"], "no-store")

        status, body, _ = self._request("GET", "/api/ear-tuner")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), self.plugin.ear_tuner)

        status, body, _ = self._request("GET", "/api/service-status")
        self.assertEqual(status, 200)
        self.assertFalse(json.loads(body)["backend_running"])

        payload = {"enabled": True, "left_mode": 4}
        status, body, _ = self._request(
            "POST",
            "/api/ear-tuner",
            json.dumps(payload),
            {"Content-Type": "application/json"},
        )
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {"applied": True})
        self.assertEqual(self.plugin.applied_values, [payload])

        self.plugin.apply_result = False
        status, body, _ = self._request("POST", "/api/ear-tuner", json.dumps(payload))
        self.assertEqual(status, 503)
        self.assertEqual(json.loads(body), {"applied": False})

        status, body, _ = self._request("POST", "/api/unknown")
        self.assertEqual(status, 404)

        for invalid_body in ("", "not json", "[]", "null"):
            status, body, _ = self._request("POST", "/api/ear-tuner", invalid_body)
            self.assertEqual(status, 400)
            self.assertIn("error", json.loads(body))

        too_large = " " * (16 * 1024 + 1)
        status, body, _ = self._request("POST", "/api/ear-tuner", too_large)
        self.assertEqual(status, 400)
        self.assertIn("invalid request size", json.loads(body)["error"])

        status, _, _ = self._request("GET", "/missing-file")
        self.assertEqual(status, 404)

    def test_stop_runs_when_server_is_shutdown(self):
        self._start_server()

        self.server_holder[0].shutdown()
        self.thread.join(timeout=3)

        self.assertFalse(self.thread.is_alive())
        self.assertTrue(self.plugin.stopped)

    def test_all_plugin_calls_share_one_event_loop(self):
        self._start_server()
        self._request("GET", "/api/ear-tuner")
        self._request("GET", "/api/service-status")
        self._request("POST", "/api/ear-tuner", '{"enabled": true}')
        self.server_holder[0].shutdown()
        self.thread.join(timeout=3)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(len(self.plugin.loops), 5)
        self.assertTrue(all(loop is self.plugin.loops[0] for loop in self.plugin.loops))

    def test_shutdown_finishes_inflight_plugin_calls_before_stopping_the_loop(self):
        entered = threading.Event()
        finish = threading.Event()
        completed = threading.Event()

        async def apply(value):
            entered.set()
            await asyncio.to_thread(finish.wait)
            completed.set()
            return True

        self.plugin.set_ear_tuner = apply
        self._start_server()
        request = threading.Thread(
            target=self._request,
            args=("POST", "/api/ear-tuner", '{"enabled": true}'),
        )
        request.start()
        try:
            self.assertTrue(entered.wait(timeout=1))
            self.server_holder[0].shutdown()
            self.assertFalse(self.plugin.stopped)
        finally:
            finish.set()
            request.join(timeout=2)
            self.thread.join(timeout=3)
        self.assertTrue(completed.is_set())
        self.assertTrue(self.plugin.stopped)
        self.assertFalse(request.is_alive())
        self.assertFalse(self.thread.is_alive())

    def test_copyfile_ignores_client_disconnects(self):
        self._start_server()
        handler_class = self.server_holder[0].RequestHandlerClass
        handler = object.__new__(handler_class)

        class DisconnectedClient:
            def write(self, _data):
                raise BrokenPipeError("client disconnected")

        handler.copyfile(io.BytesIO(b"content"), DisconnectedClient())

    def test_keyboard_interrupt_still_closes_server_and_stops_plugin(self):
        fake_server = Mock()
        fake_server.serve_forever.side_effect = KeyboardInterrupt
        with patch(
            "ear_tuner.desktop_server.ThreadingHTTPServer", return_value=fake_server
        ):
            run_desktop_server(str(self.root), lambda: self.plugin, "127.0.0.1", 1234)

        fake_server.server_close.assert_called_once()
        self.assertTrue(self.plugin.stopped)

    def test_startup_restore_error_does_not_create_http_server(self):
        class BrokenPlugin(FakePlugin):
            async def restore(self):
                raise RuntimeError("restore failed")

        with self.assertRaisesRegex(RuntimeError, "restore failed"):
            run_desktop_server(str(self.root), BrokenPlugin, "127.0.0.1", 0)
        self.assertFalse(self.server_holder)


if __name__ == "__main__":
    unittest.main()
