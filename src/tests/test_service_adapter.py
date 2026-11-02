import asyncio
import os
import unittest
from unittest.mock import AsyncMock, Mock, call, patch

from ear_tuner.service import AudioRoutingScriptAdapter


class AudioRoutingScriptAdapterTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.logger = Mock()
        self.service = AudioRoutingScriptAdapter("/plugin", self.logger)

    def test_base_environment_filters_and_supplies_runtime_defaults(self):
        with (
            patch.dict(
                os.environ, {"HOME": "/home/user", "SECRET": "hidden"}, clear=True
            ),
            patch("ear_tuner.service.os.getuid", return_value=1000),
            patch("ear_tuner.service.os.path.realpath", side_effect=lambda path: path),
        ):
            environment = self.service._base_execution_environment()

        self.assertEqual(environment["HOME"], "/home/user")
        self.assertEqual(environment["XDG_RUNTIME_DIR"], "/run/user/1000")
        self.assertEqual(
            environment["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/run/user/1000/bus"
        )
        self.assertNotIn("SECRET", environment)
        self.logger.warning.assert_not_called()

    def test_base_environment_warns_for_unexpected_uid_and_keeps_realpath(self):
        with (
            patch.dict(os.environ, {"XDG_RUNTIME_DIR": "/run/user/501"}, clear=True),
            patch("ear_tuner.service.os.getuid", return_value=501),
            patch(
                "ear_tuner.service.os.path.realpath", return_value="/private/run/501"
            ),
        ):
            environment = self.service._base_execution_environment()

        self.assertEqual(environment["XDG_RUNTIME_DIR"], "/private/run/501")
        self.logger.warning.assert_called_once()

    def test_base_environment_ignores_realpath_oserror(self):
        with (
            patch.dict(os.environ, {"XDG_RUNTIME_DIR": "/run/user/1000"}, clear=True),
            patch("ear_tuner.service.os.getuid", return_value=1000),
            patch(
                "ear_tuner.service.os.path.realpath", side_effect=OSError("bad path")
            ),
        ):
            environment = self.service._base_execution_environment()

        self.assertEqual(environment["XDG_RUNTIME_DIR"], "/run/user/1000")

    async def test_execution_environment_returns_filtered_environment(self):
        environment = {"PATH": "/fake"}
        with patch.object(
            self.service, "_base_execution_environment", return_value=environment
        ) as base_environment:
            result = await self.service.execution_environment()

        self.assertIs(result, environment)
        base_environment.assert_called_once_with()

    async def test_execute_logs_output_and_returns_true(self):
        process = Mock()
        process.communicate = AsyncMock(return_value=(b"first\nsecond\n", b""))
        process.returncode = 0
        with (
            patch.object(
                self.service,
                "execution_environment",
                new_callable=AsyncMock,
                return_value={},
            ),
            patch(
                "ear_tuner.service.asyncio.create_subprocess_exec",
                new_callable=AsyncMock,
                return_value=process,
            ) as create,
        ):
            self.assertTrue(await self.service.execute("start"))

        create.assert_awaited_once()
        self.assertEqual(
            self.logger.info.call_args_list,
            [call("service.sh: %s", "first"), call("service.sh: %s", "second")],
        )

    async def test_execute_logs_oserror_and_returns_false(self):
        with (
            patch.object(
                self.service,
                "execution_environment",
                new_callable=AsyncMock,
                return_value={},
            ),
            patch(
                "ear_tuner.service.asyncio.create_subprocess_exec",
                new_callable=AsyncMock,
                side_effect=OSError("not found"),
            ),
        ):
            self.assertFalse(await self.service.execute("start"))

        self.logger.error.assert_called_once()

    async def test_execute_logs_nonzero_status_and_decodes_stderr_safely(self):
        process = Mock()
        process.communicate = AsyncMock(return_value=(b"", b"failure\xff"))
        process.returncode = 7
        with (
            patch.object(
                self.service,
                "execution_environment",
                new_callable=AsyncMock,
                return_value={},
            ),
            patch(
                "ear_tuner.service.asyncio.create_subprocess_exec",
                new_callable=AsyncMock,
                return_value=process,
            ),
        ):
            self.assertFalse(await self.service.execute("start"))

        self.logger.error.assert_called_once()
        self.assertIn("failure", self.logger.error.call_args.args[-1])

    async def test_cancelled_execute_waits_for_the_lifecycle_command_to_finish(self):
        started = asyncio.Event()
        finish = asyncio.Event()

        async def communicate():
            started.set()
            await finish.wait()
            return b"", b""

        process = Mock()
        process.communicate = communicate
        process.returncode = 0
        with (
            patch.object(
                self.service, "execution_environment", AsyncMock(return_value={})
            ),
            patch(
                "ear_tuner.service.asyncio.create_subprocess_exec",
                AsyncMock(return_value=process),
            ),
        ):
            task = asyncio.create_task(self.service.execute("start"))
            await asyncio.wait_for(started.wait(), 1)
            task.cancel()
            await asyncio.sleep(0)
            self.assertFalse(task.done())
            finish.set()
            with self.assertRaises(asyncio.CancelledError):
                await asyncio.wait_for(task, 1)

    async def test_check_returns_process_status_and_handles_oserror(self):
        process = Mock()
        process.wait = AsyncMock(return_value=0)
        with (
            patch.object(
                self.service,
                "execution_environment",
                new_callable=AsyncMock,
                return_value={},
            ),
            patch(
                "ear_tuner.service.asyncio.create_subprocess_exec",
                new_callable=AsyncMock,
                return_value=process,
            ),
        ):
            self.assertTrue(await self.service.check("output-ready"))

        process.wait.return_value = 3
        with (
            patch.object(
                self.service,
                "execution_environment",
                new_callable=AsyncMock,
                return_value={},
            ),
            patch(
                "ear_tuner.service.asyncio.create_subprocess_exec",
                new_callable=AsyncMock,
                return_value=process,
            ),
        ):
            self.assertFalse(await self.service.check("output-ready"))

        with (
            patch.object(
                self.service,
                "execution_environment",
                new_callable=AsyncMock,
                return_value={},
            ),
            patch(
                "ear_tuner.service.asyncio.create_subprocess_exec",
                new_callable=AsyncMock,
                side_effect=OSError("missing executable"),
            ),
        ):
            self.assertFalse(await self.service.check("output-ready"))

    async def test_output_ready_forwards_expected_command(self):
        with patch.object(
            self.service, "check", new_callable=AsyncMock, return_value=True
        ) as check:
            self.assertTrue(await self.service.output_ready())

        check.assert_awaited_once_with("output-ready")

    async def test_current_output_returns_resolved_sink(self):
        process = Mock()
        process.communicate = AsyncMock(return_value=(b"headphones\n", b""))
        process.returncode = 0
        with (
            patch.object(
                self.service,
                "execution_environment",
                new_callable=AsyncMock,
                return_value={},
            ),
            patch(
                "ear_tuner.service.asyncio.create_subprocess_exec",
                new_callable=AsyncMock,
                return_value=process,
            ),
        ):
            self.assertEqual(await self.service.current_output(), "headphones")

    async def test_current_output_ignores_unresolved_sink(self):
        process = Mock()
        process.communicate = AsyncMock(return_value=(b"@DEFAULT_SINK@\n", b""))
        process.returncode = 0
        with (
            patch.object(
                self.service,
                "execution_environment",
                new_callable=AsyncMock,
                return_value={},
            ),
            patch(
                "ear_tuner.service.asyncio.create_subprocess_exec",
                new_callable=AsyncMock,
                return_value=process,
            ),
        ):
            self.assertIsNone(await self.service.current_output())


if __name__ == "__main__":
    unittest.main()
