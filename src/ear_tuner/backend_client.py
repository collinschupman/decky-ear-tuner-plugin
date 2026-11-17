import asyncio
import http.client

from ear_tuner.control_credentials import control_request
from ear_tuner.settings import EarTunerSettings


class BackendClient:
    """Client for the local native Ear Tuner control endpoint."""

    def __init__(self, control_api_url: str, logger):
        self._control_api_url = control_api_url
        self._logger = logger

    async def apply_ear_tuner(self, value: EarTunerSettings) -> bool:
        right_mode = (
            value["right_mode"] if value["separate_channels"] else value["left_mode"]
        )
        right_level = (
            value["right_level"] if value["separate_channels"] else value["left_level"]
        )
        url = (
            f"{self._control_api_url}?focus_enabled={int(value['enabled'])}"
            f"&left_mode={value['left_mode']}&left_level={value['left_level']}"
            f"&right_mode={right_mode}&right_level={right_level}"
            "&focus_bypass=0"
        )
        try:
            await self._request(url, timeout=2)
            return True
        except (OSError, ValueError, http.client.HTTPException) as exc:
            self._logger.warning("Could not push Ear Tuner settings: %s", exc)
            return False

    async def is_healthy(self) -> bool:
        try:
            await self._request(f"{self._control_api_url}?left=0&right=0", timeout=1)
            return True
        except (OSError, ValueError, http.client.HTTPException) as exc:
            self._logger.warning("Native backend health check failed: %s", exc)
            return False

    @staticmethod
    async def _request(url: str, timeout: int) -> None:
        await asyncio.to_thread(control_request, url, timeout)
