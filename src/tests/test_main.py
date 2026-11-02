import asyncio
import builtins
import os
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import AsyncMock, Mock, patch

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

import main
from ear_tuner.settings import DEFAULT_EAR_TUNER, normalize_ear_tuner
from main import Plugin


class PluginImportTests(unittest.TestCase):
    def test_main_imports_local_package_when_loaded_by_file_path(self):
        loader = """
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("decky_plugin_main", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
"""

        with tempfile.TemporaryDirectory() as working_directory:
            completed = subprocess.run(
                [sys.executable, "-I", "-c", loader, main.__file__],
                cwd=working_directory,
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_decky_import_fallback_creates_dummy_runtime(self):
        original_import = builtins.__import__

        def import_without_decky(name, *args, **kwargs):
            if name == "decky":
                raise ModuleNotFoundError("No module named 'decky'")
            return original_import(name, *args, **kwargs)

        script_directory = os.path.dirname(os.path.abspath(main.__file__))
        original_sys_path = sys.path[:]
        sys.path[:] = [entry for entry in sys.path if entry != script_directory]
        try:
            self.assertNotIn(script_directory, sys.path)
            with patch("builtins.__import__", side_effect=import_without_decky):
                namespace = runpy.run_path(main.__file__, run_name="decky_main_test")
        finally:
            sys.path[:] = original_sys_path

        dummy_decky = namespace["decky"]
        self.assertTrue(hasattr(dummy_decky, "logger"))
        self.assertTrue(dummy_decky.DECKY_HOME.endswith("/.decky"))
        self.assertIsNone(dummy_decky.migrate_logs("legacy.log"))
        self.assertIsNone(dummy_decky.migrate_settings("legacy-settings"))
        self.assertIsNone(dummy_decky.migrate_runtime("legacy-runtime"))
        dummy_decky.logger.info("info")
        dummy_decky.logger.warning("warning")
        dummy_decky.logger.error("error")


class NormalizeEarTunerTests(unittest.TestCase):
    def test_string_boolean_values_are_coerced_safely(self):
        value = {
            "enabled": "false",
            "separate_channels": "0",
            "left_mode": "99",
            "left_level": "0",
            "right_mode": 4,
            "right_level": "5",
        }

        normalized = normalize_ear_tuner(value)

        self.assertFalse(normalized["enabled"])
        self.assertFalse(normalized["separate_channels"])
        self.assertEqual(normalized["left_mode"], 4)
        self.assertEqual(normalized["left_level"], 1)
        self.assertEqual(normalized["right_mode"], 4)
        self.assertEqual(normalized["right_level"], 3)

    def test_unknown_values_fall_back_to_defaults(self):
        normalized = normalize_ear_tuner(
            {
                "enabled": None,
                "separate_channels": "not-a-bool",
                "left_mode": -100,
                "left_level": 99,
                "right_mode": "abc",
                "right_level": "abc",
            }
        )

        self.assertFalse(normalized["enabled"])
        self.assertFalse(normalized["separate_channels"])
        self.assertEqual(normalized["left_mode"], 0)
        self.assertEqual(normalized["left_level"], 3)
        self.assertEqual(normalized["right_mode"], 2)
        self.assertEqual(normalized["right_level"], 2)

    def test_truthy_and_falsy_string_aliases_are_normalized(self):
        normalized = normalize_ear_tuner(
            {
                "enabled": "yes",
                "separate_channels": "off",
                "left_mode": "1",
                "left_level": "3",
                "right_mode": "0",
                "right_level": "2",
            }
        )

        self.assertTrue(normalized["enabled"])
        self.assertFalse(normalized["separate_channels"])
        self.assertEqual(normalized["left_mode"], 1)
        self.assertEqual(normalized["left_level"], 3)
        self.assertEqual(normalized["right_mode"], 0)
        self.assertEqual(normalized["right_level"], 2)

    def test_non_dict_payload_gets_default_state(self):
        self.assertEqual(normalize_ear_tuner(None), DEFAULT_EAR_TUNER)
        self.assertEqual(normalize_ear_tuner([1, 2, 3]), DEFAULT_EAR_TUNER)


class PluginLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        main.settings = main.SettingsManager(
            name="settings-test",
            settings_directory=self.tmpdir.name,
        )
        main.settings.read()

    def tearDown(self):
        self.tmpdir.cleanup()

    def test_enable_uses_running_service_without_headphones_check(self):
        async def run_test():
            plugin = Plugin()
            with (
                patch.object(
                    main.audio_routing, "execute", AsyncMock(return_value=True)
                ) as service_exec,
                patch.object(
                    main.backend_client,
                    "apply_ear_tuner",
                    AsyncMock(return_value=True),
                ) as push,
            ):
                result = await plugin.set_ear_tuner(
                    {
                        "enabled": True,
                        "separate_channels": False,
                        "left_mode": 1,
                        "left_level": 3,
                        "right_mode": 3,
                        "right_level": 1,
                    }
                )
                self.assertTrue(result)
                self.assertEqual(service_exec.await_count, 0)
                self.assertEqual(push.await_count, 1)

        __import__("asyncio").run(run_test())


class MainModuleHelperTests(unittest.TestCase):
    def test_ensure_directory_exists_creates_path_and_ignores_oserror(self):
        with tempfile.TemporaryDirectory() as tempdir:
            path = os.path.join(tempdir, "logs", "nested")
            main.ensure_directory_exists(path)
            self.assertTrue(os.path.isdir(path))

        with patch("main.os.makedirs", side_effect=OSError("read-only")) as makedirs:
            main.ensure_directory_exists("/unavailable")
        makedirs.assert_called_once_with("/unavailable", exist_ok=True)

    def test_detect_plugin_dir_uses_existing_candidate_or_fallback(self):
        original_script_directory = main.script_directory
        try:
            with tempfile.TemporaryDirectory() as tempdir:
                main.script_directory = os.path.join(tempdir, "vendor", "plugin")
                candidate = os.path.abspath(
                    os.path.join(
                        main.script_directory,
                        os.pardir,
                        os.pardir,
                        "logs",
                        main.plugin_dir_basename,
                    )
                )
                os.makedirs(candidate)

                self.assertEqual(main.detect_plugin_dir("logs", "/fallback"), candidate)
                self.assertEqual(
                    main.detect_plugin_dir("settings", "/fallback"), "/fallback"
                )
        finally:
            main.script_directory = original_script_directory


class PluginRuntimeTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        main.settings = main.SettingsManager(
            name="runtime-test", settings_directory=self.tempdir.name
        )
        main.settings.read()
        self.plugin = Plugin()

    def tearDown(self):
        self.tempdir.cleanup()

    async def test_main_restores_state_and_starts_background_task(self):
        self.plugin.stop_event.set()
        self.plugin.background_tasks = AsyncMock()
        with patch.object(
            self.plugin._ear_tuner_coordinator, "restore", new_callable=AsyncMock
        ) as restore:
            await self.plugin._main()
            await self.plugin._background_task

        self.assertFalse(self.plugin.stop_event.is_set())
        restore.assert_awaited_once()
        self.plugin.background_tasks.assert_awaited_once()

    async def test_unload_stops_audio_service_without_background_task(self):
        with patch.object(
            self.plugin, "stop", new_callable=AsyncMock, return_value=True
        ) as stop:
            await self.plugin._unload()

        self.assertTrue(self.plugin.stop_event.is_set())
        stop.assert_awaited_once_with()

    async def test_unload_cancels_background_task_before_stopping_audio(self):
        async def wait_forever():
            await asyncio.Event().wait()

        self.plugin._background_task = asyncio.create_task(wait_forever())
        with patch.object(self.plugin, "stop", new_callable=AsyncMock) as stop:
            await self.plugin._unload()

        self.assertTrue(self.plugin._background_task.cancelled())
        stop.assert_awaited_once_with()

    async def test_unload_warns_when_backend_cleanup_fails(self):
        with (
            patch.object(
                self.plugin, "stop", new_callable=AsyncMock, return_value=False
            ),
            patch.object(main.decky.logger, "warning") as warning,
        ):
            await self.plugin._unload()

        warning.assert_called_once_with(
            "Ear Tuner: backend cleanup failed during unload"
        )

    async def test_uninstall_delegates_to_application_service(self):
        with patch.object(
            self.plugin._ear_tuner_coordinator, "uninstall", new_callable=AsyncMock
        ) as uninstall:
            await self.plugin._uninstall()

        uninstall.assert_awaited_once()

    async def test_migration_uses_legacy_paths_under_decky_home(self):
        decky = main.decky
        try:
            main.decky = Mock(
                DECKY_USER_HOME="/home/player",
                DECKY_HOME="/home/player/.decky",
            )
            main.decky.logger = Mock()
            plugin = Plugin()

            await plugin._migration()

            main.decky.migrate_logs.assert_called_once_with(
                "/home/player/.config/decky-asymmetrical-hearing/plugin.log"
            )
            main.decky.migrate_settings.assert_called_once_with(
                "/home/player/.config/decky-asymmetrical-hearing"
            )
            main.decky.migrate_runtime.assert_called_once_with(
                "/home/player/.decky/decky-asymmetrical-hearing"
            )
        finally:
            main.decky = decky

    async def test_background_loop_waits_for_stop_event(self):
        with patch.object(
            self.plugin, "_health_check", new_callable=AsyncMock
        ) as check:
            task = asyncio.create_task(self.plugin.background_tasks())
            await asyncio.sleep(0)
            self.plugin.stop_event.set()
            await asyncio.wait_for(task, timeout=1)

        check.assert_awaited_once()

    async def test_background_loop_continues_after_timeout(self):
        async def stop_after_health_check():
            self.plugin.stop_event.set()

        async def timeout_after_closing_wait(awaitable, timeout):
            awaitable.close()
            raise asyncio.TimeoutError

        with (
            patch.object(
                self.plugin,
                "_health_check",
                new_callable=AsyncMock,
                side_effect=stop_after_health_check,
            ) as check,
            patch("main.asyncio.wait_for", side_effect=timeout_after_closing_wait),
        ):
            await self.plugin.background_tasks()

        check.assert_awaited_once()

    async def test_background_loop_logs_errors_and_recovers(self):
        calls = 0

        async def health_check():
            nonlocal calls
            calls += 1
            if calls == 1:
                raise RuntimeError("transient")
            self.plugin.stop_event.set()

        with (
            patch.object(self.plugin, "_health_check", side_effect=health_check),
            patch.object(main.decky.logger, "error") as log_error,
        ):
            await asyncio.wait_for(self.plugin.background_tasks(), timeout=1)

        self.assertEqual(calls, 2)
        log_error.assert_called_once_with(
            "Background task error: %s", unittest.mock.ANY
        )

    async def test_background_loop_exits_on_cancellation(self):
        async def cancel_after_closing_wait(awaitable, timeout):
            awaitable.close()
            raise asyncio.CancelledError

        with (
            patch.object(self.plugin, "_health_check", new_callable=AsyncMock),
            patch("main.asyncio.wait_for", side_effect=cancel_after_closing_wait),
        ):
            await self.plugin.background_tasks()

        self.assertTrue(self.plugin.stop_event.is_set() is False)

    async def test_health_check_delegates(self):
        with patch.object(
            self.plugin._ear_tuner_coordinator, "health_check", new_callable=AsyncMock
        ) as health_check:
            await self.plugin._health_check()

        health_check.assert_awaited_once()

    async def test_public_rpc_methods_delegate(self):
        ear_tuner = {"enabled": True}
        status = {"backend_running": True}
        with (
            patch.object(
                self.plugin._ear_tuner_coordinator,
                "set_ear_tuner",
                new_callable=AsyncMock,
                return_value=True,
            ) as set_focus,
            patch.object(
                self.plugin._ear_tuner_coordinator,
                "get_ear_tuner",
                new_callable=AsyncMock,
                return_value=ear_tuner,
            ) as get_focus,
            patch.object(
                self.plugin._ear_tuner_coordinator,
                "get_service_status",
                new_callable=AsyncMock,
                return_value=status,
            ) as get_status,
            patch.object(
                self.plugin._ear_tuner_coordinator, "restore", new_callable=AsyncMock
            ) as restore,
            patch.object(
                self.plugin._ear_tuner_coordinator,
                "stop",
                new_callable=AsyncMock,
                return_value=True,
            ) as stop,
        ):
            await self.plugin.restore()
            self.assertTrue(await self.plugin.stop())
            self.assertTrue(await self.plugin.set_ear_tuner(ear_tuner))
            self.assertEqual(await self.plugin.get_ear_tuner(), ear_tuner)
            self.assertEqual(await self.plugin.get_service_status(), status)

        set_focus.assert_awaited_once_with(ear_tuner)
        get_focus.assert_awaited_once()
        get_status.assert_awaited_once()
        restore.assert_awaited_once()
        stop.assert_awaited_once()

    async def test_desktop_server_wrapper_forwards_arguments(self):
        with patch("ear_tuner.desktop_server.run_desktop_server") as run_server:
            main.run_desktop_server("0.0.0.0", 8123)

        run_server.assert_called_once_with(
            main.script_directory, Plugin, "0.0.0.0", 8123
        )

    async def test_module_entrypoint_starts_desktop_server(self):
        script_directory = os.path.dirname(os.path.abspath(main.__file__))
        inserted = script_directory in sys.path
        if inserted:
            sys.path.remove(script_directory)
        try:
            with patch("ear_tuner.desktop_server.run_desktop_server") as run_server:
                runpy.run_path(main.__file__, run_name="__main__")
        finally:
            if inserted:
                sys.path.insert(0, script_directory)

        run_server.assert_called_once_with(
            main.script_directory, unittest.mock.ANY, "127.0.0.1", 5173
        )

    async def test_log_frontend_event_bounds_event_and_payload(self):
        logger = main.decky.logger
        try:
            main.decky.logger = Mock()
            plugin = Plugin()
            self.assertTrue(
                await plugin.log_frontend_event("e" * 100, {"b": 1, "a": object()})
            )

            message, event_name, payload = main.decky.logger.info.call_args.args
            self.assertEqual(message, "Frontend diagnostic [%s]: %s")
            self.assertEqual(event_name, "e" * 80)
            self.assertTrue(payload.startswith('{"a": "<object object at '))
        finally:
            main.decky.logger = logger

    async def test_log_frontend_event_wraps_non_dict_and_truncates_payload(self):
        logger = main.decky.logger
        try:
            main.decky.logger = Mock()
            plugin = Plugin()
            self.assertTrue(await plugin.log_frontend_event("event", "x" * 3000))

            _, event_name, payload = main.decky.logger.info.call_args.args
            self.assertEqual(event_name, "event")
            self.assertEqual(len(payload), 2000)
            self.assertTrue(payload.startswith('{"value": "'))
        finally:
            main.decky.logger = logger

    def test_disabling_ear_tuner_keeps_service_running(self):
        async def run_test():
            plugin = Plugin()
            with (
                patch.object(
                    main.audio_routing, "execute", AsyncMock(return_value=True)
                ) as service_exec,
                patch.object(
                    main.backend_client,
                    "apply_ear_tuner",
                    AsyncMock(return_value=True),
                ) as push,
            ):
                result = await plugin.set_ear_tuner(
                    {
                        "enabled": False,
                        "separate_channels": False,
                        "left_mode": 0,
                        "left_level": 1,
                        "right_mode": 0,
                        "right_level": 1,
                    }
                )
                self.assertTrue(result)
                service_exec.assert_not_awaited()
                push.assert_awaited_once()

        __import__("asyncio").run(run_test())

    def test_mode_change_is_applied_without_headphones_check(self):
        async def run_test():
            plugin = Plugin()
            current = {
                "enabled": True,
                "separate_channels": False,
                "left_mode": 2,
                "left_level": 2,
                "right_mode": 2,
                "right_level": 2,
            }
            updated = {**current, "left_mode": 4, "right_mode": 4}
            main.settings.setSetting("ear_tuner", current)

            with (
                patch.object(
                    main.audio_routing, "execute", AsyncMock()
                ) as service_exec,
                patch.object(
                    main.backend_client, "apply_ear_tuner", AsyncMock()
                ) as push,
            ):
                result = await plugin.set_ear_tuner(updated)

            self.assertTrue(result)
            self.assertEqual(await plugin.get_ear_tuner(), updated)
            service_exec.assert_not_awaited()
            push.assert_awaited_once_with(updated)

        __import__("asyncio").run(run_test())

    def test_mode_change_preserves_saved_settings_when_backend_update_fails(self):
        async def run_test():
            plugin = Plugin()
            current = {
                "enabled": True,
                "separate_channels": False,
                "left_mode": 2,
                "left_level": 2,
                "right_mode": 2,
                "right_level": 2,
            }
            main.settings.setSetting("ear_tuner", current)

            with (
                patch.object(
                    main.audio_routing, "execute", AsyncMock(return_value=True)
                ) as service_exec,
                patch.object(
                    main.backend_client,
                    "apply_ear_tuner",
                    AsyncMock(return_value=False),
                ),
            ):
                result = await plugin.set_ear_tuner(
                    {
                        "enabled": True,
                        "separate_channels": True,
                        "left_mode": 4,
                        "left_level": 1,
                        "right_mode": 1,
                        "right_level": 3,
                    }
                )
                self.assertFalse(result)
                self.assertEqual(await plugin.get_ear_tuner(), current)
                service_exec.assert_not_awaited()

        __import__("asyncio").run(run_test())


if __name__ == "__main__":
    unittest.main()
