import asyncio
import urllib.request

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
        )
        try:
            await self._request(url, timeout=2)
            return True
        except OSError as exc:
            self._logger.warning("Could not push Ear Tuner settings: %s", exc)
            return False

    async def is_healthy(self) -> bool:
        try:
            await self._request(f"{self._control_api_url}?left=0&right=0", timeout=1)
            return True
        except OSError:
            return False

    @staticmethod
    async def _request(url: str, timeout: int) -> None:
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(
            None,
            lambda: urllib.request.urlopen(url, timeout=timeout),  # noqa: S310
        )
