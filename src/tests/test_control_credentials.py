import http.server
import os
import stat
import tempfile
import threading
import unittest
import urllib.error
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import Mock, patch

from ear_tuner.control_credentials import (
    control_request,
    ensure_control_token,
    read_control_token,
)


class ControlCredentialTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.runtime = Path(self.temporary.name).resolve()
        self.environment = patch.dict(
            os.environ, {"XDG_RUNTIME_DIR": str(self.runtime), "FLATPAK_ID": ""}
        )
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_credentials_are_private_atomic_and_reused_on_restart(self):
        token_path = ensure_control_token()
        token = read_control_token()
        self.assertEqual(len(token), 64)
        self.assertEqual(stat.S_IMODE(token_path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(token_path.parent.stat().st_mode), 0o700)
        self.assertEqual(ensure_control_token(), token_path)
        self.assertEqual(read_control_token(), token)
        self.assertEqual(list(token_path.parent.iterdir()), [token_path])

    def test_missing_unsafe_symlinked_and_malformed_credentials_fail_closed(self):
        with self.assertRaises(FileNotFoundError):
            read_control_token()
        path = ensure_control_token()
        for value in (b"", b"x" * 64 + b"\n", b"a" * 64, b"a" * 65 + b"\n"):
            path.write_bytes(value)
            with self.assertRaises(ValueError):
                read_control_token()
            with self.assertRaises(ValueError):
                ensure_control_token()
        path.write_bytes(b"a" * 64 + b"\n")
        path.chmod(0o644)
        with self.assertRaises(PermissionError):
            read_control_token()
        path.chmod(0o600)
        saved = path.with_suffix(".saved")
        path.rename(saved)
        path.symlink_to(saved)
        with self.assertRaises(OSError):
            read_control_token()
        path.unlink()
        saved.rename(path)
        path.parent.chmod(0o755)
        with self.assertRaises(PermissionError):
            read_control_token()

    def test_concurrent_provisioning_publishes_one_complete_token(self):
        def provision(_index):
            return ensure_control_token(), read_control_token()

        with ThreadPoolExecutor(max_workers=8) as executor:
            results = list(executor.map(provision, range(16)))
        self.assertEqual(len(set(results)), 1)
        path, token = results[0]
        self.assertEqual(path.read_text(), token + "\n")
        self.assertEqual(list(path.parent.iterdir()), [path])

    def test_shared_runtime_or_foreign_ownership_is_rejected(self):
        self.runtime.chmod(0o755)
        with self.assertRaises(PermissionError):
            ensure_control_token()
        self.runtime.chmod(0o700)
        with patch(
            "ear_tuner.control_credentials.os.getuid", return_value=os.getuid() + 1
        ):
            with self.assertRaises(PermissionError):
                ensure_control_token()

    def test_flatpak_reads_credentials_on_host_without_putting_secret_in_arguments(
        self,
    ):
        with (
            patch.dict(os.environ, {"FLATPAK_ID": "test.flatpak"}),
            patch(
                "ear_tuner.control_credentials.shutil.which",
                return_value="/bin/flatpak-spawn",
            ),
            patch(
                "ear_tuner.control_credentials.subprocess.run",
                return_value=Mock(stdout="a" * 64 + "\n"),
            ) as run,
        ):
            self.assertEqual(read_control_token(), "a" * 64)
            self.assertEqual(
                run.call_args.args[0][:3], ["flatpak-spawn", "--host", "python3"]
            )
            self.assertNotIn("a" * 64, " ".join(run.call_args.args[0]))

    def test_authenticated_requests_reload_token_and_do_not_follow_redirects_or_proxies(
        self,
    ):
        path = ensure_control_token()
        expected = [read_control_token()]
        requests = []
        redirect = [False]
        marker = [True]

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                authorization = self.headers.get("Authorization")
                requests.append((self.path, authorization))
                if redirect[0]:
                    self.send_response(302)
                    self.send_header("Location", "/redirect-target")
                else:
                    self.send_response(
                        200 if authorization == "Bearer " + expected[0] else 401
                    )
                    if marker[0]:
                        self.send_header("X-Ear-Tuner-Auth", "bearer")
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"OK")

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/api/v1/control"
            with patch.dict(
                os.environ,
                {
                    "http_proxy": "http://127.0.0.1:1",
                    "HTTP_PROXY": "http://127.0.0.1:1",
                    "no_proxy": "",
                    "NO_PROXY": "",
                },
            ):
                self.assertEqual(control_request(url, 2), b"OK")
            self.assertNotIn(expected[0], requests[-1][0])
            expected[0] = "b" * 64
            path.write_text(expected[0] + "\n")
            self.assertEqual(control_request(url, 2), b"OK")
            marker[0] = False
            with self.assertRaisesRegex(ValueError, "authenticated control protocol"):
                control_request(url, 2)
            redirect[0] = True
            before = len(requests)
            with self.assertRaises(urllib.error.HTTPError):
                control_request(url, 2)
            self.assertEqual(len(requests), before + 1)
            for unsafe in (
                "https://127.0.0.1/",
                "http://example.com/",
                "http://user:pass@127.0.0.1/",
            ):
                with self.assertRaises(ValueError):
                    control_request(unsafe, 2)
        finally:
            server.shutdown()
            worker.join()
            server.server_close()
