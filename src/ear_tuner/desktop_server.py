import asyncio
import json
import os
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


def run_desktop_server(
    script_directory: str,
    create_plugin,
    host: str = "127.0.0.1",
    port: int = 5173,
) -> None:
    """Serve the standalone UI through the same plugin RPC lifecycle."""
    loop = asyncio.new_event_loop()
    worker = threading.Thread(target=loop.run_forever, name="ear-tuner-api")
    worker.start()
    try:
        _serve_desktop_server(script_directory, create_plugin, host, port, loop)
    finally:
        loop.call_soon_threadsafe(loop.stop)
        worker.join()
        loop.close()


def _serve_desktop_server(
    script_directory: str,
    create_plugin,
    host: str,
    port: int,
    loop: asyncio.AbstractEventLoop,
) -> None:
    web_root = os.path.join(script_directory, "dist-web")
    plugin = create_plugin()
    asyncio.run_coroutine_threadsafe(plugin.restore(), loop).result()

    class DesktopHandler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=web_root, **kwargs)

        def end_headers(self) -> None:
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def send_json(self, status: int, value: object) -> None:
            body = json.dumps(value).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def copyfile(self, source, outputfile) -> None:
            try:
                super().copyfile(source, outputfile)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self) -> None:
            request_path = urlparse(self.path).path
            if request_path == "/api/ear-tuner":
                self.send_json(
                    200,
                    asyncio.run_coroutine_threadsafe(
                        plugin.get_ear_tuner(), loop
                    ).result(),
                )
                return
            if request_path == "/api/service-status":
                self.send_json(
                    200,
                    asyncio.run_coroutine_threadsafe(
                        plugin.get_service_status(), loop
                    ).result(),
                )
                return
            super().do_GET()

        def do_POST(self) -> None:
            request_path = urlparse(self.path).path
            if request_path != "/api/ear-tuner":
                self.send_error(404)
                return
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if size <= 0 or size > 16 * 1024:
                    raise ValueError("invalid request size")
                value = json.loads(self.rfile.read(size))
                if not isinstance(value, dict):
                    raise ValueError("settings must be an object")
                applied = asyncio.run_coroutine_threadsafe(
                    plugin.set_ear_tuner(value), loop
                ).result()
                self.send_json(200 if applied else 503, {"applied": applied})
            except (json.JSONDecodeError, TypeError, ValueError) as exc:
                self.send_json(400, {"error": str(exc)})

    server = None
    try:
        server = ThreadingHTTPServer((host, port), DesktopHandler)
        server.daemon_threads = False
        print(f"Ear Tuner desktop UI: http://{host}:{port}/", flush=True)
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if server is not None:
            server.server_close()
        asyncio.run_coroutine_threadsafe(plugin.stop(), loop).result()
