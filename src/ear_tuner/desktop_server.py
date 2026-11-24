import asyncio
import json
import os
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable, Protocol, runtime_checkable
from urllib.parse import urlparse

from ear_tuner.settings import EarTunerSettings


class DesktopPlugin(Protocol):
    async def restore(self) -> None: ...

    async def stop(self) -> bool: ...

    async def get_ear_tuner(self) -> EarTunerSettings: ...

    async def set_ear_tuner(self, value: dict[str, object]) -> bool: ...

    async def get_service_status(self) -> dict[str, bool]: ...


@runtime_checkable
class DesktopAudioControl(Protocol):
    async def set_audio_service(self, running: bool) -> bool: ...


def run_desktop_server(
    script_directory: str,
    create_plugin: Callable[[], DesktopPlugin],
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
    create_plugin: Callable[[], DesktopPlugin],
    host: str,
    port: int,
    loop: asyncio.AbstractEventLoop,
) -> None:
    web_root = os.path.join(script_directory, "dist-web")
    plugin = create_plugin()

    class DesktopHandler(SimpleHTTPRequestHandler):
        timeout = 5

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
            if request_path == "/api/audio-service" and isinstance(
                plugin, DesktopAudioControl
            ):
                status = asyncio.run_coroutine_threadsafe(
                    plugin.get_service_status(), loop
                ).result()
                self.send_json(200, {"running": status["backend_running"]})
                return
            super().do_GET()

        def do_POST(self) -> None:
            request_path = urlparse(self.path).path
            audio_control = request_path == "/api/audio-service" and isinstance(
                plugin, DesktopAudioControl
            )
            if request_path != "/api/ear-tuner" and not audio_control:
                self.send_error(404)
                return
            try:
                if (
                    audio_control
                    and self.headers.get_content_type() != "application/json"
                ):
                    raise ValueError("audio service requires application/json")
                size = int(self.headers.get("Content-Length", "0"))
                if size <= 0 or size > 16 * 1024:
                    raise ValueError("invalid request size")
                value = json.loads(self.rfile.read(size))
                if not isinstance(value, dict):
                    raise ValueError("settings must be an object")
                if audio_control and isinstance(plugin, DesktopAudioControl):
                    if set(value) != {"running"} or not isinstance(
                        value["running"], bool
                    ):
                        raise ValueError("running must be a boolean")
                    operation = plugin.set_audio_service(value["running"])
                else:
                    operation = plugin.set_ear_tuner(value)
                applied = asyncio.run_coroutine_threadsafe(operation, loop).result()
                self.send_json(200 if applied else 503, {"applied": applied})
            except TimeoutError:
                self.send_json(408, {"error": "request body timed out"})
            except (json.JSONDecodeError, TypeError, ValueError) as exc:
                self.send_json(400, {"error": str(exc)})

    server = None
    startup = asyncio.run_coroutine_threadsafe(plugin.restore(), loop)
    try:
        startup.result()
        server = ThreadingHTTPServer((host, port), DesktopHandler)
        server.daemon_threads = False
        print(f"Ear Tuner desktop UI: http://{host}:{port}/", flush=True)
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if not startup.done():
            startup.cancel()
        if server is not None:
            server.server_close()
        asyncio.run_coroutine_threadsafe(plugin.stop(), loop).result()
