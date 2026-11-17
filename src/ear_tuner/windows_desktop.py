"""Standalone Windows host for the shared browser UI and application service."""

import argparse
import asyncio
import contextlib
import logging
import os
import signal
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

from ear_tuner.application_service import EarTunerCoordinator
from ear_tuner.desktop_server import run_desktop_server
from ear_tuner.settings import EarTunerSettings, SettingsManager
from ear_tuner.windows_service import WindowsAudioService, WindowsBackendClient


class WindowsDesktopPlugin:
    def __init__(
        self,
        settings: SettingsManager,
        routing: WindowsAudioService,
        logger: logging.Logger,
    ):
        self._routing = routing
        self._logger = logger
        self._coordinator = EarTunerCoordinator(
            settings, logger, routing, WindowsBackendClient(routing, logger)
        )
        self._monitor: asyncio.Task[None] | None = None

    async def restore(self) -> None:
        # Check readiness before restore can alter persisted settings on startup failure.
        if not await self._routing.execute("start"):
            raise RuntimeError(f"Audio startup failed. See {self._routing.log_path}")
        await self._coordinator.restore()
        if not await self._routing.output_ready():
            raise RuntimeError(f"Settings restore failed. See {self._routing.log_path}")
        self._monitor = asyncio.create_task(self._monitor_audio())
        self._monitor.add_done_callback(self._monitor_finished)

    def _monitor_finished(self, task: asyncio.Task[None]) -> None:
        if not task.cancelled() and (error := task.exception()) is not None:
            self._logger.error("Audio health monitor failed: %s", error)

    async def _monitor_audio(self) -> None:
        while True:
            await asyncio.sleep(2)
            await self._coordinator.health_check()

    async def stop(self) -> bool:
        try:
            if self._monitor:
                self._monitor.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await self._monitor
        finally:
            stopped = await self._coordinator.stop()
        return stopped

    async def get_ear_tuner(self) -> EarTunerSettings:
        return await self._coordinator.get_ear_tuner()

    async def set_ear_tuner(self, value: dict[str, object]) -> bool:
        return await self._coordinator.set_ear_tuner(value)

    async def get_service_status(self) -> dict[str, bool]:
        return await self._coordinator.get_service_status()


@contextlib.contextmanager
def _instance_lock(path: Path) -> Iterator[None]:
    import msvcrt

    with path.open("a+b") as lock:
        lock.seek(0, os.SEEK_END)
        if lock.tell() == 0:
            lock.write(b"\0")
            lock.flush()
        lock.seek(0)
        msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        try:
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--input", help="VB-CABLE recording endpoint UID")
    parser.add_argument("--output", help="Physical headphone output UID (not its name)")
    parser.add_argument("--port", type=int, default=5173, help="Local web UI port")
    arguments = parser.parse_args()
    if sys.platform != "win32":
        parser.error("This launcher requires Windows")
    if not 1 <= arguments.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    root = Path(__file__).resolve().parents[2]
    executable = (
        root / "native" / "build-current" / "bin" / "Release" / "ear_tuner_windows.exe"
    )
    if not executable.is_file():
        parser.error(
            "Build the Release ear_tuner_windows target first; see docs/WINDOWS.md"
        )
    if arguments.list_devices:
        return subprocess.run(
            [str(executable), "--list-devices"], check=False
        ).returncode
    if not arguments.input or not arguments.output:
        parser.error("--input and --output are required; use --list-devices for UIDs")
    if not (root / "dist-web" / "index.html").is_file():
        parser.error("Build the desktop UI first: pnpm run build:web")

    if not (local_data := os.environ.get("LOCALAPPDATA")):
        parser.error("LOCALAPPDATA is not set")
    data = Path(local_data) / "Ear Tuner"
    logs = data / "logs"
    data.mkdir(parents=True, exist_ok=True)
    logs.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        level=logging.INFO,
        format="[%(asctime)s][%(levelname)s] %(message)s",
        handlers=[
            logging.StreamHandler(),
            logging.FileHandler(logs / "desktop.log", encoding="utf-8"),
        ],
    )
    logger = logging.getLogger("ear-tuner-windows")
    try:
        with _instance_lock(data / "desktop.lock"):
            settings = SettingsManager("settings", str(data))
            settings.read()
            routing = WindowsAudioService(
                executable,
                arguments.input,
                arguments.output,
                logs / "native.log",
                logger,
            )
            logger.info(
                "Route Windows playback to CABLE Input; restore physical headphones on exit. "
                "Ear Tuner does not change system routing. Press Ctrl+C to stop."
            )
            previous_handler = signal.signal(signal.SIGTERM, signal.default_int_handler)
            try:
                run_desktop_server(
                    str(root),
                    lambda: WindowsDesktopPlugin(settings, routing, logger),
                    port=arguments.port,
                )
            finally:
                signal.signal(signal.SIGTERM, previous_handler)
    except (OSError, RuntimeError) as error:
        logger.error("Desktop startup/runtime or instance lock failed: %s", error)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
