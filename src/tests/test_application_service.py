import asyncio
import unittest
from unittest.mock import AsyncMock, Mock

from ear_tuner.application_service import EarTunerCoordinator
from ear_tuner.settings import DEFAULT_EAR_TUNER


def focus_settings(**overrides):
    return {**DEFAULT_EAR_TUNER, **overrides}


class EarTunerCoordinatorTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.settings = Mock()
        self.settings.getSetting.return_value = focus_settings()
        self.logger = Mock()
        self.service = Mock()
        self.service.execute = AsyncMock(return_value=True)
        self.service.output_ready = AsyncMock(return_value=True)
        self.service.current_output = AsyncMock(return_value="speakers")
        self.backend = Mock()
        self.backend.apply_ear_tuner = AsyncMock(return_value=True)
        self.backend.is_healthy = AsyncMock(return_value=True)
        self.audio = EarTunerCoordinator(
            self.settings, self.logger, self.service, self.backend
        )

    async def test_get_ear_tuner_normalizes_stored_settings(self):
        self.settings.getSetting.return_value = {
            "enabled": "yes",
            "left_mode": "4",
            "left_level": 99,
        }

        result = await self.audio.get_ear_tuner()

        self.assertEqual(
            result, focus_settings(enabled=True, left_mode=4, left_level=3)
        )
        self.settings.getSetting.assert_called_once_with("ear_tuner", None)

    async def test_get_ear_tuner_reads_legacy_settings(self):
        self.settings.getSetting.side_effect = [None, {"enabled": True}]

        result = await self.audio.get_ear_tuner()

        self.assertTrue(result["enabled"])
        self.settings.getSetting.assert_has_calls(
            [
                unittest.mock.call("ear_tuner", None),
                unittest.mock.call("audio_focus", {}),
            ]
        )

    async def test_restore_starts_bypassed_service_when_ear_tuner_is_disabled(self):
        await self.audio.restore()

        self.service.execute.assert_awaited_once_with("start")
        self.backend.apply_ear_tuner.assert_awaited_once_with(focus_settings())

    async def test_restore_starts_and_applies_enabled_settings(self):
        value = focus_settings(enabled=True, separate_channels=True)
        self.settings.getSetting.return_value = value

        await self.audio.restore()

        self.service.execute.assert_awaited_once_with("start")
        self.backend.apply_ear_tuner.assert_awaited_once_with(value)
        self.settings.setSetting.assert_not_called()

    async def test_restore_disables_and_stops_when_start_fails(self):
        self.settings.getSetting.return_value = focus_settings(enabled=True)
        self.service.execute = AsyncMock(side_effect=[False, True])

        await self.audio.restore()

        stored = self.settings.setSetting.call_args.args[1]
        self.assertFalse(stored["enabled"])
        self.service.execute.assert_has_awaits(
            [unittest.mock.call("start"), unittest.mock.call("stop")]
        )
        self.backend.apply_ear_tuner.assert_not_awaited()

    async def test_restore_disables_and_stops_when_backend_apply_fails(self):
        value = focus_settings(enabled=True)
        self.settings.getSetting.return_value = value
        self.backend.apply_ear_tuner.return_value = False

        await self.audio.restore()

        self.assertFalse(self.settings.setSetting.call_args.args[1]["enabled"])
        self.service.execute.assert_has_awaits(
            [unittest.mock.call("start"), unittest.mock.call("stop")]
        )

    async def test_stop_and_uninstall_forward_service_results(self):
        self.service.execute.side_effect = [True, False]

        self.assertTrue(await self.audio.stop())
        self.assertFalse(await self.audio.uninstall())
        self.service.execute.assert_has_awaits(
            [unittest.mock.call("stop"), unittest.mock.call("uninstall")]
        )

    async def test_disabling_persists_without_stopping_and_logs_mode_change(self):
        value = focus_settings(enabled=False, left_mode=1)

        self.assertTrue(await self.audio.set_ear_tuner(value))

        self.settings.setSetting.assert_called_once_with("ear_tuner", value)
        self.service.execute.assert_not_awaited()
        self.backend.apply_ear_tuner.assert_awaited_once_with(value)
        self.logger.info.assert_any_call("Focus mode saved while Ear Tuner is disabled")

    async def test_disabling_active_processing_keeps_audio_graph_running(self):
        previous = focus_settings(enabled=True)
        disabled = focus_settings(enabled=False)
        self.settings.getSetting.return_value = previous

        self.assertTrue(await self.audio.set_ear_tuner(disabled))

        self.backend.apply_ear_tuner.assert_awaited_once_with(disabled)
        self.service.execute.assert_not_awaited()

    async def test_disabling_without_mode_change_does_not_log_mode_save(self):
        value = focus_settings(enabled=False)

        await self.audio.set_ear_tuner(value)

        self.assertFalse(
            any(
                "Focus mode saved" in str(call)
                for call in self.logger.info.call_args_list
            )
        )

    async def test_enable_after_backend_failure_leaves_stable_graph_running(self):
        self.settings.getSetting.return_value = focus_settings(enabled=False)
        self.backend.apply_ear_tuner.return_value = False

        self.assertFalse(await self.audio.set_ear_tuner(focus_settings(enabled=True)))
        self.service.execute.assert_not_awaited()
        self.settings.setSetting.assert_not_called()

    async def test_backend_failure_logs_mode_change_when_previous_was_disabled(self):
        self.settings.getSetting.return_value = focus_settings(enabled=False)
        self.backend.apply_ear_tuner.return_value = False
        self.service.execute.return_value = True

        self.assertFalse(
            await self.audio.set_ear_tuner(focus_settings(enabled=True, left_mode=3))
        )

        self.logger.warning.assert_called_once_with(
            "Ear Tuner settings were not applied; keeping saved settings"
        )

    async def test_successful_non_mode_update_persists_without_mode_log(self):
        value = focus_settings(enabled=True, left_level=3)

        self.assertTrue(await self.audio.set_ear_tuner(value))

        self.settings.setSetting.assert_called_once_with("ear_tuner", value)
        self.logger.info.assert_not_called()

    async def test_health_check_keeps_disabled_graph_healthy(self):
        self.settings.getSetting.return_value = focus_settings(enabled=False)
        self.backend.is_healthy.return_value = False

        await self.audio.health_check()

        self.service.execute.assert_awaited_once_with("start")
        self.backend.apply_ear_tuner.assert_awaited_once_with(
            focus_settings(enabled=False)
        )

    async def test_health_check_skips_restart_when_backend_is_healthy(self):
        self.settings.getSetting.return_value = focus_settings(enabled=True)

        await self.audio.health_check()

        self.backend.is_healthy.assert_awaited_once()
        self.service.execute.assert_not_awaited()

    async def test_health_check_restarts_and_reapplies_after_output_change(self):
        value = focus_settings(enabled=True, left_mode=2, right_mode=2)
        self.settings.getSetting.return_value = value
        self.audio._current_output = "speakers"
        self.service.current_output.return_value = "headphones"

        await self.audio.health_check()

        self.service.execute.assert_awaited_once_with("start")
        self.backend.apply_ear_tuner.assert_awaited_once_with(value)
        self.assertEqual(self.audio._current_output, "headphones")
        self.logger.info.assert_called_once_with(
            "Audio output changed: %s -> %s; restarting filter",
            "speakers",
            "headphones",
        )

    async def test_health_check_restarts_service_and_reapplies_settings(self):
        value = focus_settings(enabled=True, separate_channels=True)
        self.settings.getSetting.return_value = value
        self.backend.is_healthy.return_value = False

        await self.audio.health_check()

        self.service.execute.assert_awaited_once_with("start")
        self.backend.apply_ear_tuner.assert_awaited_once_with(value)
        self.logger.warning.assert_called_once()

    async def test_service_status_reports_output_readiness(self):
        self.service.output_ready.return_value = False
        self.backend.is_healthy.return_value = True

        self.assertEqual(
            await self.audio.get_service_status(),
            {
                "backend_running": True,
                "output_ready": False,
            },
        )
        self.service.output_ready.assert_awaited_once()

    async def test_recovery_cannot_overwrite_a_concurrent_disable(self):
        stored = focus_settings(enabled=True)
        self.settings.getSetting.side_effect = lambda key, default=None: dict(stored)
        self.settings.setSetting.side_effect = lambda key, value: stored.update(value)
        self.audio._current_output = "speakers"
        discovery_started = asyncio.Event()
        discovery_resumed = asyncio.Event()

        async def discover_output():
            discovery_started.set()
            await discovery_resumed.wait()
            return "headphones"

        self.service.current_output.side_effect = discover_output
        recovery = asyncio.create_task(self.audio.health_check())
        await asyncio.wait_for(discovery_started.wait(), 1)
        disabled = focus_settings(enabled=False)
        update = asyncio.create_task(self.audio.set_ear_tuner(disabled))
        await asyncio.sleep(0)
        self.assertFalse(update.done())
        discovery_resumed.set()
        await asyncio.wait_for(recovery, 1)
        self.assertTrue(await asyncio.wait_for(update, 1))
        self.assertFalse(stored["enabled"])
        self.assertEqual(self.backend.apply_ear_tuner.await_args.args[0], disabled)
        await self.audio.health_check()
        self.assertEqual(self.backend.apply_ear_tuner.await_count, 2)

    async def test_failed_updates_never_replace_acknowledged_settings(self):
        self.settings.getSetting.return_value = focus_settings(enabled=True)
        self.backend.apply_ear_tuner.return_value = False
        for value in (
            focus_settings(enabled=False),
            focus_settings(enabled=True, left_mode=4),
        ):
            with self.subTest(value=value):
                self.assertFalse(await self.audio.set_ear_tuner(value))
                self.settings.setSetting.assert_not_called()

    async def test_stop_and_uninstall_prevent_subsequent_recovery(self):
        for command in ("stop", "uninstall"):
            with self.subTest(command=command):
                self.service.execute.reset_mock()
                self.backend.is_healthy.reset_mock()
                await getattr(self.audio, command)()
                await self.audio.health_check()
                self.service.execute.assert_awaited_once_with(command)
                self.backend.is_healthy.assert_not_awaited()
                self.assertFalse(
                    await self.audio.set_ear_tuner(focus_settings(enabled=True))
                )


if __name__ == "__main__":
    unittest.main()
