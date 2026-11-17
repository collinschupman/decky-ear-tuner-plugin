import asyncio
import http.client
import json
import logging
import subprocess
import time
import urllib.request
import uuid
from pathlib import Path
from typing import cast

from ear_tuner.backend_client import BackendClient
from ear_tuner.settings import EarTunerSettings

DIAGNOSTICS_URL = "http://127.0.0.1:19285/api/v1/diagnostics"
CONTROL_URL = "http://127.0.0.1:19285/api/v1/control"


class MacOSAudioService:
    """Own only the native child process created by this desktop instance."""

    def __init__(
        self,
        executable: Path,
        input_uid: str,
        output_uid: str,
        log_path: Path,
        logger: logging.Logger,
    ):
        self.executable = executable
        self.input_uid = input_uid
        self.output_uid = output_uid
        self.log_path = log_path
        self.logger = logger
        self._process: asyncio.subprocess.Process | None = None
        self._session = ""
        self._lifecycle_lock = asyncio.Lock()

    def _read_diagnostics(self) -> dict[str, object]:
        with urllib.request.urlopen(DIAGNOSTICS_URL, timeout=1) as response:
            value = json.load(response)
        if not isinstance(value, dict):
            raise ValueError("Native diagnostics must be an object")
        return cast(dict[str, object], value)

    async def output_ready(self) -> bool:
        process = self._process
        session = self._session
        if process is None or process.returncode is not None:
            return False
        try:
            value = await asyncio.to_thread(self._read_diagnostics)
        except (OSError, ValueError, http.client.HTTPException) as error:
            self.logger.debug("Native diagnostics unavailable: %s", error)
            return False
        return (
            self._process is process
            and process.returncode is None
            and value.get("platform") == "coreaudio"
            and value.get("session") == session
            and value.get("pid") == process.pid
            and value.get("audio_ready") is True
        )

    async def current_output(self) -> str | None:
        return self.output_uid if await self.output_ready() else None

    async def execute(self, command: str) -> bool:
        async with self._lifecycle_lock:
            return await self._execute(command)

    async def _execute(self, command: str) -> bool:
        if command in ("stop", "uninstall"):
            return await self._stop()
        if command != "start":
            raise ValueError(f"Unknown macOS lifecycle command: {command}")
        if await self.output_ready():
            return True
        if not await self._stop():
            return False
        self._session = uuid.uuid4().hex
        try:
            with self.log_path.open("ab") as log:
                launch = asyncio.create_task(
                    asyncio.create_subprocess_exec(
                        str(self.executable),
                        "--input",
                        self.input_uid,
                        "--output",
                        self.output_uid,
                        "--session",
                        self._session,
                        stdin=subprocess.DEVNULL,
                        stdout=log,
                        stderr=log,
                    )
                )
                try:
                    process = await asyncio.shield(launch)
                    self._process = process
                except asyncio.CancelledError:
                    self._process = await launch
                    raise
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if await self.output_ready():
                    return True
                if process.returncode is not None:
                    break
                await asyncio.sleep(0.1)
            self.logger.error(
                "Core Audio backend failed to become ready. See %s", self.log_path
            )
        except OSError as error:
            self.logger.error("Could not start Core Audio backend: %s", error)
        except asyncio.CancelledError:
            await self._stop()
            raise
        await self._stop()
        return False

    async def _stop(self) -> bool:
        process = self._process
        if process is None:
            return True
        if process.returncode is None:
            try:
                process.terminate()
                try:
                    await asyncio.wait_for(process.wait(), timeout=5)
                except asyncio.TimeoutError:
                    self.logger.warning(
                        "Core Audio backend did not stop; killing child"
                    )
                    process.kill()
                    await asyncio.wait_for(process.wait(), timeout=5)
            except ProcessLookupError:
                await process.wait()
            except (OSError, asyncio.TimeoutError) as error:
                self.logger.error("Could not stop Core Audio backend: %s", error)
                return False
        self._process = None
        return True


class MacOSBackendClient(BackendClient):
    """Do not send settings to another instance sharing the control port."""

    def __init__(self, routing: MacOSAudioService, logger: logging.Logger):
        super().__init__(CONTROL_URL, logger)
        self._routing = routing

    async def is_healthy(self) -> bool:
        return await self._routing.output_ready() and await super().is_healthy()

    async def apply_ear_tuner(self, value: EarTunerSettings) -> bool:
        if not await self._routing.output_ready():
            self._logger.warning("Owned Core Audio backend is not ready")
            return False
        return await super().apply_ear_tuner(value)
