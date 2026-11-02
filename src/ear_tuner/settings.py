import json
import os
from typing import TypedDict


class SettingsManager:
    """Fallback JSON settings store used outside the Decky runtime."""

    def __init__(self, name: str, settings_directory: str | None = None):
        self.settings_path = os.path.join(settings_directory or ".", f"{name}.json")
        self._settings: dict = {}

    def read(self) -> None:
        try:
            with open(self.settings_path, "r", encoding="utf-8") as file:
                self._settings = json.load(file)
        except (FileNotFoundError, json.JSONDecodeError):
            self._settings = {}

    def _write(self) -> None:
        os.makedirs(os.path.dirname(self.settings_path), exist_ok=True)
        with open(self.settings_path, "w", encoding="utf-8") as file:
            json.dump(self._settings, file, indent=2)

    def getSetting(self, key: str, default=None):
        return self._settings.get(key, default)

    def setSetting(self, key: str, value) -> None:
        self._settings[key] = value
        self._write()


class EarTunerSettings(TypedDict):
    enabled: bool
    separate_channels: bool
    left_mode: int
    left_level: int
    right_mode: int
    right_level: int


DEFAULT_EAR_TUNER: EarTunerSettings = {
    "enabled": False,
    "separate_channels": False,
    "left_mode": 2,
    "left_level": 2,
    "right_mode": 2,
    "right_level": 2,
}


def clamp_int(value, minimum: int, maximum: int, fallback: int) -> int:
    try:
        return max(minimum, min(maximum, int(value)))
    except (TypeError, ValueError):
        return fallback


def coerce_bool(value, fallback: bool) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        if value in (0, 1):
            return bool(value)
        return fallback
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in {"1", "true", "yes", "on"}:
            return True
        if normalized in {"0", "false", "no", "off", ""}:
            return False
    return fallback


def normalize_ear_tuner(value) -> EarTunerSettings:
    source = value if isinstance(value, dict) else {}
    return {
        "enabled": coerce_bool(
            source.get("enabled", DEFAULT_EAR_TUNER["enabled"]),
            DEFAULT_EAR_TUNER["enabled"],
        ),
        "separate_channels": coerce_bool(
            source.get("separate_channels", DEFAULT_EAR_TUNER["separate_channels"]),
            DEFAULT_EAR_TUNER["separate_channels"],
        ),
        "left_mode": clamp_int(source.get("left_mode"), 0, 4, 2),
        "left_level": clamp_int(source.get("left_level"), 1, 3, 2),
        "right_mode": clamp_int(source.get("right_mode"), 0, 4, 2),
        "right_level": clamp_int(source.get("right_level"), 1, 3, 2),
    }
