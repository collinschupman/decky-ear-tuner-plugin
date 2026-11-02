import asyncio
import unittest
from unittest.mock import AsyncMock, Mock, patch

from ear_tuner.backend_client import BackendClient
from ear_tuner.settings import DEFAULT_EAR_TUNER


class BackendClientTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.logger = Mock()
        self.client = BackendClient(
            "http://127.0.0.1:19285/api/v1/control", self.logger
        )

    async def test_apply_serializes_shared_channel_settings(self):
        value = {
            **DEFAULT_EAR_TUNER,
            "enabled": True,
            "left_mode": 4,
            "left_level": 3,
            "right_mode": 1,
            "right_level": 1,
        }
        with patch.object(BackendClient, "_request", new_callable=AsyncMock) as request:
            self.assertTrue(await self.client.apply_ear_tuner(value))

        request.assert_awaited_once_with(
            "http://127.0.0.1:19285/api/v1/control?focus_enabled=1"
            "&left_mode=4&left_level=3&right_mode=4&right_level=3",
            timeout=2,
        )
        self.logger.warning.assert_not_called()

    async def test_apply_serializes_separate_channel_settings(self):
        value = {
            **DEFAULT_EAR_TUNER,
            "enabled": True,
            "separate_channels": True,
            "left_mode": 4,
            "left_level": 3,
            "right_mode": 1,
            "right_level": 2,
        }
        with patch.object(BackendClient, "_request", new_callable=AsyncMock) as request:
            self.assertTrue(await self.client.apply_ear_tuner(value))

        request.assert_awaited_once_with(
            "http://127.0.0.1:19285/api/v1/control?focus_enabled=1"
            "&left_mode=4&left_level=3&right_mode=1&right_level=2",
            timeout=2,
        )

    async def test_apply_returns_false_and_warns_on_os_error(self):
        with patch.object(
            BackendClient,
            "_request",
            new_callable=AsyncMock,
            side_effect=OSError("connection refused"),
        ):
            self.assertFalse(await self.client.apply_ear_tuner(DEFAULT_EAR_TUNER))

        self.logger.warning.assert_called_once_with(
            "Could not push Ear Tuner settings: %s",
            unittest.mock.ANY,
        )

    async def test_health_check_returns_true_when_request_succeeds(self):
        with patch.object(BackendClient, "_request", new_callable=AsyncMock) as request:
            self.assertTrue(await self.client.is_healthy())

        request.assert_awaited_once_with(
            "http://127.0.0.1:19285/api/v1/control?left=0&right=0", timeout=1
        )

    async def test_health_check_returns_false_on_os_error(self):
        with patch.object(
            BackendClient,
            "_request",
            new_callable=AsyncMock,
            side_effect=OSError("timeout"),
        ):
            self.assertFalse(await self.client.is_healthy())

        self.logger.warning.assert_not_called()

    async def test_request_runs_urlopen_in_executor(self):
        response = object()
        future = asyncio.get_running_loop().create_future()
        future.set_result(None)
        with patch("ear_tuner.backend_client.asyncio.get_running_loop") as get_loop:
            executor = get_loop.return_value.run_in_executor
            executor.return_value = future

            await BackendClient._request("http://localhost/control", timeout=7)
            callback = executor.call_args.args[1]
            with patch(
                "ear_tuner.backend_client.urllib.request.urlopen",
                return_value=response,
            ) as urlopen:
                callback()

        executor.assert_called_once()
        urlopen.assert_called_once_with("http://localhost/control", timeout=7)


if __name__ == "__main__":
    unittest.main()
