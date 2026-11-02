import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ear_tuner.settings import (
    DEFAULT_EAR_TUNER,
    SettingsManager,
    clamp_int,
    coerce_bool,
    normalize_ear_tuner,
)


class SettingsManagerTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.directory = Path(self.tempdir.name) / "nested" / "settings"
        self.manager = SettingsManager("plugin", str(self.directory))

    def tearDown(self):
        self.tempdir.cleanup()

    def test_missing_file_reads_as_empty_settings(self):
        self.manager._settings = {"stale": True}

        self.manager.read()

        self.assertEqual(self.manager.getSetting("missing"), None)
        self.assertEqual(self.manager._settings, {})

    def test_invalid_json_resets_settings_to_empty(self):
        self.directory.mkdir(parents=True)
        Path(self.manager.settings_path).write_text("{bad", encoding="utf-8")
        self.manager._settings = {"stale": True}

        self.manager.read()

        self.assertEqual(self.manager._settings, {})

    def test_read_propagates_unexpected_os_errors(self):
        with patch("builtins.open", side_effect=PermissionError("denied")):
            with self.assertRaisesRegex(PermissionError, "denied"):
                self.manager.read()

    def test_set_writes_nested_json_and_can_be_reloaded(self):
        value = {"ear_tuner": DEFAULT_EAR_TUNER}

        self.manager.setSetting("ear_tuner", value["ear_tuner"])

        self.assertTrue(self.directory.is_dir())
        self.assertEqual(
            json.loads(Path(self.manager.settings_path).read_text(encoding="utf-8")),
            value,
        )
        reloaded = SettingsManager("plugin", str(self.directory))
        reloaded.read()
        self.assertEqual(reloaded.getSetting("ear_tuner"), value["ear_tuner"])

    def test_get_setting_uses_default_only_when_key_is_missing(self):
        self.manager._settings = {"saved": None}

        self.assertEqual(self.manager.getSetting("missing", "fallback"), "fallback")
        self.assertIsNone(self.manager.getSetting("saved", "fallback"))

    def test_write_errors_propagate_after_in_memory_update(self):
        with patch.object(self.manager, "_write", side_effect=OSError("disk full")):
            with self.assertRaisesRegex(OSError, "disk full"):
                self.manager.setSetting("value", 3)

        self.assertEqual(self.manager.getSetting("value"), 3)


class SettingsNormalizationEdgeTests(unittest.TestCase):
    def test_clamp_int_accepts_bounds_and_uses_fallback_for_bad_values(self):
        self.assertEqual(clamp_int("1", 1, 3, 2), 1)
        self.assertEqual(clamp_int("3", 1, 3, 2), 3)
        self.assertEqual(clamp_int(object(), 1, 3, 2), 2)
        self.assertEqual(clamp_int("bad", 1, 3, 2), 2)

    def test_coerce_bool_accepts_numeric_and_string_aliases(self):
        for value, expected in ((True, True), (False, False), (1, True), (0, False)):
            with self.subTest(value=value):
                self.assertIs(coerce_bool(value, not expected), expected)

        for value in ("1", "true", "YES", " on "):
            with self.subTest(value=value):
                self.assertTrue(coerce_bool(value, False))
        for value in ("0", "false", "NO", " off ", ""):
            with self.subTest(value=value):
                self.assertFalse(coerce_bool(value, True))

    def test_invalid_numeric_boolean_values_use_fallback(self):
        self.assertTrue(coerce_bool(2, True))
        self.assertFalse(coerce_bool(-1, False))
        self.assertTrue(coerce_bool(None, True))
        self.assertFalse(coerce_bool([], False))

    def test_normalize_uses_per_field_fallbacks_for_missing_keys(self):
        self.assertEqual(normalize_ear_tuner({}), DEFAULT_EAR_TUNER)


if __name__ == "__main__":
    unittest.main()
