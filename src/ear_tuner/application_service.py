import asyncio
from typing import Protocol

from ear_tuner.settings import EarTunerSettings, normalize_ear_tuner

LEGACY_SETTINGS_KEY = "audio_focus"


class SettingsStore(Protocol):
    def getSetting(self, key: str, default=None): ...

    def setSetting(self, key: str, value) -> None: ...


class Logger(Protocol):
    def info(self, message: str, *args) -> None: ...

    def warning(self, message: str, *args) -> None: ...


class AudioRoutingControl(Protocol):
    async def execute(self, command: str) -> bool: ...

    async def output_ready(self) -> bool: ...

    async def current_output(self) -> str | None: ...


class BackendControl(Protocol):
    async def apply_ear_tuner(self, value: EarTunerSettings) -> bool: ...

    async def is_healthy(self) -> bool: ...


class EarTunerCoordinator:
    """Coordinates persisted settings with routing and backend adapters."""

    def __init__(
        self,
        settings: SettingsStore,
        logger: Logger,
        audio_routing: AudioRoutingControl,
        backend: BackendControl,
    ):
        self._settings = settings
        self._logger = logger
        self._audio_routing = audio_routing
        self._backend = backend
        self._current_output: str | None = None
        self._lifecycle_lock = asyncio.Lock()
        self._stopped = False

    async def get_ear_tuner(self) -> EarTunerSettings:
        stored = self._settings.getSetting("ear_tuner", None)
        if stored is None:
            stored = self._settings.getSetting(LEGACY_SETTINGS_KEY, {})
        return normalize_ear_tuner(stored)

    async def restore(self) -> None:
        async with self._lifecycle_lock:
            self._stopped = False
            await self._restore()

    async def _restore(self) -> None:
        ear_tuner = await self.get_ear_tuner()
        self._logger.info("Ear Tuner: starting audio service")
        if await self._audio_routing.execute(
            "start"
        ) and await self._backend.apply_ear_tuner(ear_tuner):
            self._current_output = await self._audio_routing.current_output()
            return
        ear_tuner["enabled"] = False
        self._settings.setSetting("ear_tuner", ear_tuner)
        await self._audio_routing.execute("stop")

    async def stop(self) -> bool:
        async with self._lifecycle_lock:
            self._stopped = True
            return await self._audio_routing.execute("stop")

    async def uninstall(self) -> bool:
        async with self._lifecycle_lock:
            self._stopped = True
            return await self._audio_routing.execute("uninstall")

    async def set_ear_tuner(self, value: dict) -> bool:
        async with self._lifecycle_lock:
            if self._stopped:
                self._logger.warning(
                    "Cannot apply settings while the audio service is stopped"
                )
                return False
            return await self._set_ear_tuner(value)

    async def _set_ear_tuner(self, value: dict) -> bool:
        normalized = normalize_ear_tuner(value)
        previous = await self.get_ear_tuner()
        mode_changed = (
            normalized["left_mode"] != previous["left_mode"]
            or normalized["right_mode"] != previous["right_mode"]
        )
        if mode_changed:
            self._logger.info(
                "Focus mode apply requested: left=%d->%d right=%d->%d "
                "separate=%s enabled=%s",
                previous["left_mode"],
                normalized["left_mode"],
                previous["right_mode"],
                normalized["right_mode"],
                normalized["separate_channels"],
                normalized["enabled"],
            )
        if not normalized["enabled"]:
            if not await self._backend.apply_ear_tuner(normalized):
                self._logger.warning("Could not confirm that Ear Tuner was disabled")
                return False
            self._settings.setSetting("ear_tuner", normalized)
            if mode_changed:
                self._logger.info("Focus mode saved while Ear Tuner is disabled")
            return True
        if not await self._backend.apply_ear_tuner(normalized):
            self._logger.warning(
                "Ear Tuner settings were not applied; keeping saved settings"
            )
            return False
        self._settings.setSetting("ear_tuner", normalized)
        if mode_changed:
            self._logger.info("Focus mode applied and persisted successfully")
        return True

    async def health_check(self) -> None:
        async with self._lifecycle_lock:
            if not self._stopped:
                await self._health_check()

    async def _health_check(self) -> None:
        ear_tuner = await self.get_ear_tuner()
        output = await self._audio_routing.current_output()
        backend_healthy = await self._backend.is_healthy()
        output_changed = (
            output is not None
            and self._current_output is not None
            and output != self._current_output
        )
        if backend_healthy and not output_changed:
            if output is not None:
                self._current_output = output
            return
        if output_changed:
            self._logger.info(
                "Audio output changed: %s -> %s; restarting filter",
                self._current_output,
                output,
            )
        else:
            self._logger.warning("Backend unreachable - attempting restart")
        if await self._audio_routing.execute(
            "start"
        ) and await self._backend.apply_ear_tuner(ear_tuner):
            self._current_output = output

    async def get_service_status(self) -> dict[str, bool]:
        return {
            "backend_running": await self._backend.is_healthy(),
            "output_ready": await self._audio_routing.output_ready(),
        }
