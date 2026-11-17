"""Private per-login credentials for the native loopback API."""

import http.client
import os
import secrets
import shutil
import stat
import subprocess
import sys
import urllib.request
from pathlib import Path
from urllib.parse import urlsplit


def _validate_metadata(info: os.stat_result, directory: bool) -> None:
    expected_mode = 0o700 if directory else 0o600
    valid_type = stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)
    if (
        not valid_type
        or info.st_uid != os.getuid()
        or stat.S_IMODE(info.st_mode) != expected_mode
    ):
        raise PermissionError(
            "Control credentials must be owned by this user with private permissions"
        )


def _directory() -> int:
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    base = os.open(runtime, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        _validate_metadata(os.fstat(base), True)
        try:
            os.mkdir("ear-tuner", mode=0o700, dir_fd=base)
        except FileExistsError:
            pass
        directory = os.open(
            "ear-tuner", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=base
        )
    finally:
        os.close(base)
    try:
        _validate_metadata(os.fstat(directory), True)
    except PermissionError:
        os.close(directory)
        raise
    return directory


def _read_local_token() -> str:
    directory = _directory()
    try:
        descriptor = os.open(
            "control.token",
            os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
            dir_fd=directory,
        )
    finally:
        os.close(directory)
    with os.fdopen(descriptor, "rb") as token_file:
        _validate_metadata(os.fstat(token_file.fileno()), False)
        value = token_file.read(66)
    if (
        len(value) != 65
        or value[-1:] != b"\n"
        or any(ch not in b"0123456789abcdef" for ch in value[:64])
    ):
        raise ValueError("Malformed native control credentials")
    return value[:64].decode("ascii")


def ensure_control_token() -> Path:
    directory = _directory()
    temporary = f".control-{secrets.token_hex(16)}"
    try:
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
            0o600,
            dir_fd=directory,
        )
        try:
            with os.fdopen(descriptor, "wb") as token_file:
                token_file.write((secrets.token_hex(32) + "\n").encode("ascii"))
                token_file.flush()
                os.fsync(token_file.fileno())
            try:
                os.link(
                    temporary,
                    "control.token",
                    src_dir_fd=directory,
                    dst_dir_fd=directory,
                    follow_symlinks=False,
                )
            except FileExistsError:
                pass
        finally:
            os.unlink(temporary, dir_fd=directory)
    finally:
        os.close(directory)
    _read_local_token()
    return (
        Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
        / "ear-tuner"
        / "control.token"
    )


def read_control_token() -> str:
    if os.environ.get("FLATPAK_ID"):
        if not shutil.which("flatpak-spawn"):
            raise OSError("flatpak-spawn is required to read host control credentials")
        try:
            result = subprocess.run(
                [
                    "flatpak-spawn",
                    "--host",
                    "python3",
                    str(Path(__file__).resolve()),
                    "read",
                ],
                capture_output=True,
                check=True,
                timeout=2,
                text=True,
            )
        except (subprocess.SubprocessError, OSError) as error:
            raise OSError("Could not read host control credentials") from error
        token = result.stdout.strip()
        if len(token) != 64 or any(ch not in "0123456789abcdef" for ch in token):
            raise ValueError("Malformed host control credentials")
        return token
    return _read_local_token()


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def control_request(url: str, timeout: int, *, local: bool = False) -> bytes:
    parsed = urlsplit(url)
    if (
        parsed.scheme != "http"
        or parsed.hostname != "127.0.0.1"
        or parsed.username
        or parsed.password
    ):
        raise ValueError("Native control requests must use IPv4 loopback HTTP")
    token = _read_local_token() if local else read_control_token()
    request = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())
    with opener.open(request, timeout=timeout) as response:
        if (
            response.status != 200
            or response.headers.get("X-Ear-Tuner-Auth") != "bearer"
        ):
            raise ValueError("Backend did not confirm authenticated control protocol")
        body = response.read(65537)
        if len(body) > 65536:
            raise ValueError("Native control response exceeded size limit")
        return body


def main() -> int:
    try:
        if sys.argv[1:] == ["ensure"]:
            print(ensure_control_token())
        elif sys.argv[1:] == ["read"]:
            print(_read_local_token())
        elif len(sys.argv) == 3 and sys.argv[1] == "request":
            sys.stdout.buffer.write(control_request(sys.argv[2], timeout=1, local=True))
        else:
            raise ValueError("Usage: control_credentials.py {ensure|read|request URL}")
    except (OSError, ValueError, http.client.HTTPException) as error:
        print(f"Native control credentials/request failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
