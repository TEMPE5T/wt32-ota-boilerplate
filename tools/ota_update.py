#!/usr/bin/env python3
"""Push an ESP-IDF application image to a WT32-ETH01 running this boilerplate.

The device writes the image with esp_ota_begin / esp_ota_write / esp_ota_end,
reboots, and confirms itself. This script does not speak the Arduino espota
protocol.

Examples:
    python3 tools/ota_update.py 192.168.1.50 build/wt32-ota-boilerplate.bin
    python3 tools/ota_update.py --token secret --port 8080 192.168.1.50 firmware.bin
    python3 tools/ota_update.py --check-only firmware.bin
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

# ESP-IDF application layout. The first byte is the image magic. The
# application description begins after esp_image_header_t (24 bytes) and
# esp_image_segment_header_t (8 bytes), which is offset 0x20. Inside
# esp_app_desc_t, version[32] is at byte 16 and project_name[32] is at byte 48.
ESP_IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 0x20
VERSION_OFFSET = APP_DESC_OFFSET + 16
PROJECT_OFFSET = APP_DESC_OFFSET + 48
VERSION_LEN = 32
PROJECT_LEN = 32
MIN_IMAGE_BYTES = PROJECT_OFFSET + PROJECT_LEN


class ImageError(Exception):
    """The file is not an ESP-IDF application image this board should boot."""


def _cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def read_image_info(path: Path) -> tuple[bytes, str, str]:
    """Return the file bytes, project name, and version from an app image."""
    data = path.read_bytes()
    if len(data) < MIN_IMAGE_BYTES:
        raise ImageError(f"{path} is too small to be an ESP-IDF application image")
    if data[0] != ESP_IMAGE_MAGIC:
        raise ImageError(f"{path} magic is 0x{data[0]:02x}, expected 0xE9")
    magic = int.from_bytes(data[APP_DESC_OFFSET:APP_DESC_OFFSET + 4], "little")
    if magic != APP_DESC_MAGIC:
        raise ImageError(
            f"{path} application-description magic is 0x{magic:08x}, expected 0xABCD5432"
        )
    version = _cstr(data[VERSION_OFFSET:VERSION_OFFSET + VERSION_LEN])
    project = _cstr(data[PROJECT_OFFSET:PROJECT_OFFSET + PROJECT_LEN])
    if not version:
        raise ImageError(f"{path} has an empty version string")
    if not project:
        raise ImageError(f"{path} has an empty project name")
    return data, project, version


def _request(url: str, timeout: float, data: bytes | None = None, headers: dict[str, str] | None = None):
    req = urllib.request.Request(url, data=data, headers=headers or {}, method="POST" if data is not None else "GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
            return resp.status, body
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()


class _ProgressBody:
    """File-like body that reports how much of the image has been handed to the socket."""

    def __init__(self, payload: bytes) -> None:
        self._payload = payload
        self._offset = 0
        self._next = 64 * 1024

    def read(self, size: int = -1) -> bytes:
        if size is None or size < 0:
            size = len(self._payload) - self._offset
        chunk = self._payload[self._offset:self._offset + size]
        self._offset += len(chunk)
        # The HTTP client reads once more at EOF. Only report a read that moved data.
        if chunk and (self._offset >= self._next or self._offset == len(self._payload)):
            print(
                f"uploaded {self._offset} / {len(self._payload)} bytes",
                file=sys.stderr,
            )
            while self._next <= self._offset:
                self._next += 64 * 1024
        return chunk

    def __len__(self) -> int:
        return len(self._payload)


def upload(ip: str, port: int, payload: bytes, token: str, timeout: float) -> tuple[int | None, bytes]:
    headers = {
        "Content-Type": "application/octet-stream",
        "Content-Length": str(len(payload)),
    }
    if token:
        headers["X-OTA-Token"] = token
    url = f"http://{ip}:{port}/ota"
    body = _ProgressBody(payload)
    request = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()
    except (TimeoutError, socket.timeout, urllib.error.URLError, ConnectionError, OSError) as exc:
        print(f"upload connection ended: {exc}", file=sys.stderr)
        print("the board may already be rebooting; waiting for it to come back", file=sys.stderr)
        return None, b""


def poll_version(ip: str, port: int, version: str, deadline: float) -> dict:
    url = f"http://{ip}:{port}/info"
    last_error = "no response"
    while time.monotonic() < deadline:
        try:
            status, raw = _request(url, timeout=5)
        except (TimeoutError, socket.timeout, urllib.error.URLError, ConnectionError, OSError) as exc:
            last_error = str(exc)
            time.sleep(2)
            continue
        if status != 200:
            last_error = f"HTTP {status}: {raw[:200]!r}"
            time.sleep(2)
            continue
        try:
            info = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            last_error = f"bad /info body: {exc}"
            time.sleep(2)
            continue
        running = info.get("version")
        state = info.get("ota_state")
        print(
            f"device version {running} on {info.get('partition')} ({state})",
            file=sys.stderr,
        )
        if running == version and state != "pending_verify":
            return info
        last_error = f"still running {running} ({state})"
        time.sleep(2)
    raise TimeoutError(last_error)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Push a firmware .bin to a WT32-ETH01 running the OTA boilerplate."
    )
    parser.add_argument(
        "ip",
        nargs="?",
        help="Address the board is using. This is the compiled static IP when DHCP failed or is disabled.",
    )
    parser.add_argument("bin", help="Path to the application .bin produced by idf.py build")
    parser.add_argument("--port", type=int, default=8080, help="OTA HTTP port (default 8080)")
    parser.add_argument("--token", default="", help="Value of X-OTA-Token, if the board was built with one")
    parser.add_argument(
        "--timeout",
        type=int,
        default=120,
        help="Seconds to wait after the upload for the new version to leave pending_verify",
    )
    parser.add_argument(
        "--check-only",
        action="store_true",
        help="Print the image project and version, then exit without contacting a board",
    )
    args = parser.parse_args(argv)
    if not args.check_only and not args.ip:
        parser.error("ip is required unless --check-only is set")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.timeout < 1:
        parser.error("--timeout must be at least 1 second")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    path = Path(args.bin)
    if not path.is_file():
        print(f"error: {path} is not a file", file=sys.stderr)
        return 1
    try:
        payload, project, version = read_image_info(path)
    except ImageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"image {project} version {version} ({len(payload)} bytes)", file=sys.stderr)
    if args.check_only:
        return 0

    print(f"posting to http://{args.ip}:{args.port}/ota", file=sys.stderr)
    status, raw = upload(args.ip, args.port, payload, args.token, timeout=max(args.timeout, 60))
    if status is not None and status >= 400:
        text = raw.decode("utf-8", errors="replace").strip()
        print(f"error: device rejected the image (HTTP {status}): {text}", file=sys.stderr)
        return 2
    if status is not None:
        text = raw.decode("utf-8", errors="replace").strip()
        print(f"device accepted the image (HTTP {status}): {text}", file=sys.stderr)

    try:
        info = poll_version(args.ip, args.port, version, time.monotonic() + args.timeout)
    except TimeoutError as exc:
        print(f"error: new version did not become current: {exc}", file=sys.stderr)
        print("the previous image may have been restored by rollback", file=sys.stderr)
        return 3

    print(
        f"running {info.get('version')} on {info.get('partition')} "
        f"at {info.get('ip')} ({info.get('addr_mode')})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
