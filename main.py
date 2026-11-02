import asyncio
import contextlib
import json
import logging
import os
import sys

script_directory = os.path.dirname(os.path.abspath(__file__))
source_directory = os.path.join(script_directory, "src")
for import_directory in (script_directory, source_directory):
    if import_directory not in sys.path:
        sys.path.insert(0, import_directory)

from ear_tuner.application_service import EarTunerCoordinator  # noqa: E402
from ear_tuner.backend_client import BackendClient  # noqa: E402
from ear_tuner.service import AudioRoutingScriptAdapter  # noqa: E402
from ear_tuner.settings import (  # noqa: E402
    EarTunerSettings,
)
from ear_tuner.settings import (  # noqa: E402
    SettingsManager as FallbackSettingsManager,
)

plugin_dir_basename = os.path.basename(script_directory)
tmp_log_dir = os.path.join(script_directory, "tmp", "logs")
tmp_settings_dir = os.path.join(script_directory, "tmp", "config")


def ensure_directory_exists(path: str) -> None:
    try:
        os.makedirs(path, exist_ok=True)
    except OSError:
        pass


def detect_plugin_dir(kind: str, fallback: str) -> str:
    candidate = os.path.abspath(
        os.path.join(script_directory, os.pardir, os.pardir, kind, plugin_dir_basename)
    )
    return candidate if os.path.isdir(candidate) else fallback


log_dir = detect_plugin_dir("logs", tmp_log_dir)
settings_dir = detect_plugin_dir("settings", tmp_settings_dir)

log_dir = os.environ.get("DECKY_PLUGIN_LOG_DIR", log_dir)
settings_dir = os.environ.get("DECKY_PLUGIN_SETTINGS_DIR", settings_dir)

ensure_directory_exists(log_dir)
ensure_directory_exists(settings_dir)

for env_key, resolved_path in {
    "DECKY_PLUGIN_LOG_DIR": log_dir,
    "DECKY_PLUGIN_SETTINGS_DIR": settings_dir,
}.items():
    os.environ.setdefault(env_key, resolved_path)

try:
    import decky
except ModuleNotFoundError:
    logging.basicConfig(
        level=logging.INFO,
        format="[%(asctime)s][%(levelname)s]: %(message)s",
        stream=sys.stderr,
    )

    class _DummyLogger:
        def __init__(self):
            self._logger = logging.getLogger("decky-cli")

        def info(self, msg, *args):
            self._logger.info(msg, *args)

        def warning(self, msg, *args):
            self._logger.warning(msg, *args)

        def error(self, msg, *args):
            self._logger.error(msg, *args)

    class _DummyDecky:
        def __init__(self):
            self.logger = _DummyLogger()
            self.DECKY_USER_HOME = os.path.expanduser("~")
            self.DECKY_HOME = os.path.join(self.DECKY_USER_HOME, ".decky")

        def migrate_logs(self, *_args, **_kwargs):
            pass

        def migrate_settings(self, *_args, **_kwargs):
            pass

        def migrate_runtime(self, *_args, **_kwargs):
            pass

    decky = _DummyDecky()

try:
    from settings import SettingsManager  # type: ignore
except ModuleNotFoundError:
    SettingsManager = FallbackSettingsManager


settings = SettingsManager(name="settings", settings_directory=settings_dir)
settings.read()

HTTP_PORT = 19285
CONTROL_API_URL = f"http://127.0.0.1:{HTTP_PORT}/api/v1/control"
HEALTH_CHECK_INTERVAL_SECONDS = 2


audio_routing = AudioRoutingScriptAdapter(script_directory, decky.logger)
backend_client = BackendClient(CONTROL_API_URL, decky.logger)


class Plugin:
    def __init__(self):
        self._background_task: asyncio.Task | None = None
        self.stop_event = asyncio.Event()
        self._ear_tuner_coordinator = EarTunerCoordinator(
            settings,
            decky.logger,
            audio_routing,
            backend_client,
        )

    async def _main(self):
        self.stop_event.clear()
        await self._ear_tuner_coordinator.restore()
        self._background_task = asyncio.create_task(self.background_tasks())
        decky.logger.info("Ear Tuner: _main complete")

    async def restore(self) -> None:
        """Restore the persisted state for the standalone server."""
        await self._ear_tuner_coordinator.restore()

    async def stop(self) -> bool:
        """Stop the Ear Tuner service for the standalone server."""
        return await self._ear_tuner_coordinator.stop()

    async def _unload(self):
        decky.logger.info("Ear Tuner: unloading")
        self.stop_event.set()
        if self._background_task:
            self._background_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._background_task
        if not await self.stop():
            decky.logger.warning("Ear Tuner: backend cleanup failed during unload")
        decky.logger.info("Ear Tuner: unloaded")

    async def _uninstall(self):
        decky.logger.info("Ear Tuner: uninstalling")
        await self._ear_tuner_coordinator.uninstall()

    async def _migration(self):
        decky.logger.info("Ear Tuner: running legacy migrations")
        decky.migrate_logs(
            os.path.join(
                decky.DECKY_USER_HOME,
                ".config",
                "decky-asymmetrical-hearing",
                "plugin.log",
            )
        )
        decky.migrate_settings(
            os.path.join(decky.DECKY_USER_HOME, ".config", "decky-asymmetrical-hearing")
        )
        decky.migrate_runtime(
            os.path.join(decky.DECKY_HOME, "decky-asymmetrical-hearing")
        )

    async def background_tasks(self):
        decky.logger.info("Background task started")
        while not self.stop_event.is_set():
            try:
                await self._health_check()
                try:
                    await asyncio.wait_for(
                        self.stop_event.wait(), timeout=HEALTH_CHECK_INTERVAL_SECONDS
                    )
                except asyncio.TimeoutError:
                    pass
            except asyncio.CancelledError:
                break
            except Exception as exc:
                decky.logger.error("Background task error: %s", exc)
        decky.logger.info("Background task stopped")

    async def _health_check(self):
        """If the backend HTTP server is unreachable, try to restart the service."""
        await self._ear_tuner_coordinator.health_check()

    # Public RPC methods

    async def set_ear_tuner(self, value: dict) -> bool:
        """Validate, persist, and apply the complete Ear Tuner state."""
        return await self._ear_tuner_coordinator.set_ear_tuner(value)

    async def log_frontend_event(self, event: str, details: dict) -> bool:
        """Persist bounded frontend diagnostics in the Decky plugin log."""
        event_name = str(event)[:80]
        safe_details = details if isinstance(details, dict) else {"value": details}
        payload = json.dumps(safe_details, default=str, sort_keys=True)[:2000]
        decky.logger.info("Frontend diagnostic [%s]: %s", event_name, payload)
        return True

    async def get_ear_tuner(self) -> EarTunerSettings:
        """Return the persisted Ear Tuner state."""
        return await self._ear_tuner_coordinator.get_ear_tuner()

    async def get_service_status(self) -> dict:
        """Return a brief status dict for the frontend to display."""
        return await self._ear_tuner_coordinator.get_service_status()


def run_desktop_server(host: str = "127.0.0.1", port: int = 5173) -> None:
    """Serve the standalone UI with the same lifecycle used by Decky RPCs."""
    from ear_tuner.desktop_server import (
        run_desktop_server as run_desktop_http_server,
    )

    run_desktop_http_server(
        script_directory,
        Plugin,
        host,
        port,
    )


if __name__ == "__main__":
    run_desktop_server()
