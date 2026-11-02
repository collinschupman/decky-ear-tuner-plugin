import asyncio
import asyncio.subprocess
import os


class AudioRoutingScriptAdapter:
    """Async adapter for the plugin's service.sh lifecycle commands."""

    def __init__(self, script_directory: str, logger):
        self._service_script = os.path.join(script_directory, "service.sh")
        self._logger = logger

    async def execution_environment(self) -> dict[str, str]:
        return self._base_execution_environment()

    def _base_execution_environment(self) -> dict[str, str]:
        uid = os.getuid()
        if uid not in (1000, 1001):
            self._logger.warning(
                "Running subprocess as uid %s - is something wrong?", uid
            )
        allowed_keys = [
            "DBUS_SESSION_BUS_ADDRESS",
            "FLATPAK_ID",
            "HOME",
            "LANG",
            "PATH",
            "SHELL",
            "USER",
            "XDG_DATA_DIRS",
            "XDG_RUNTIME_DIR",
            "XDG_SESSION_CLASS",
            "XDG_SESSION_ID",
            "XDG_SESSION_TYPE",
        ]
        environment = {
            key: os.environ[key] for key in allowed_keys if key in os.environ
        }
        environment["XDG_RUNTIME_DIR"] = os.environ.get(
            "XDG_RUNTIME_DIR", f"/run/user/{uid}"
        )
        try:
            resolved = os.path.realpath(environment["XDG_RUNTIME_DIR"])
            if resolved:
                environment["XDG_RUNTIME_DIR"] = resolved
        except OSError:
            pass
        environment["DBUS_SESSION_BUS_ADDRESS"] = os.environ.get(
            "DBUS_SESSION_BUS_ADDRESS", f"unix:path=/run/user/{uid}/bus"
        )
        return environment

    async def execute(self, command: str) -> bool:
        try:
            process = await asyncio.create_subprocess_exec(
                self._service_script,
                command,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env=await self.execution_environment(),
            )
            completion = asyncio.create_task(process.communicate())
            try:
                stdout, stderr = await asyncio.shield(completion)
            except asyncio.CancelledError:
                self._logger.info(
                    "Waiting for service.sh %s before cancelling", command
                )
                await completion
                raise
        except OSError as exc:
            self._logger.error("Error running service.sh %s: %s", command, exc)
            return False
        if process.returncode != 0:
            self._logger.error(
                "service.sh %s failed (rc=%d): %s",
                command,
                process.returncode,
                stderr.decode(errors="ignore").strip(),
            )
            return False
        for line in stdout.decode(errors="ignore").splitlines():
            self._logger.info("service.sh: %s", line)
        return True

    async def check(self, command: str) -> bool:
        try:
            process = await asyncio.create_subprocess_exec(
                self._service_script,
                command,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL,
                env=await self.execution_environment(),
            )
            return await process.wait() == 0
        except OSError:
            return False

    async def output_ready(self) -> bool:
        return await self.check("output-ready")

    async def current_output(self) -> str | None:
        try:
            process = await asyncio.create_subprocess_exec(
                self._service_script,
                "output-id",
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL,
                env=await self.execution_environment(),
            )
            stdout, _ = await process.communicate()
        except OSError:
            return None
        output_id = stdout.decode(errors="ignore").strip()
        if process.returncode != 0 or not output_id or output_id == "@DEFAULT_SINK@":
            return None
        return output_id
