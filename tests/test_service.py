"""Container-level checks for the WPE screenshot engine's public HTTP surface.

Run from the repository root after building wpe_screenshot_engine/Dockerfile:
    python3 -m unittest discover -s tests

The tests use only a local deterministic fixture and never need an HA token.
"""

from __future__ import annotations

import http.client
import http.server
import json
import os
import random
import re
import socket
import ssl
import string
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
import uuid
import zlib
from urllib.parse import quote

IMAGE = os.environ.get("WPE_IMAGE", "wpe-screenshot-engine:dev")
ROUTE = "/lovelace/energy"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def docker(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["docker", *args], text=True, capture_output=True, check=check
    )


def reset_connection(client: socket.socket) -> None:
    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    client.close()


def request_at(port: int, path: str, *, method: str = "GET", timeout: float = 10.0):
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        method=method,
        data=b"" if method not in ("GET", "HEAD") else None,
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, response.headers, response.read()
    except urllib.error.HTTPError as error:
        try:
            return error.code, error.headers, error.read()
        finally:
            error.close()


def request(path: str, *, method: str = "GET", timeout: float = 10.0):
    return request_at(_ServiceTests.port, path, method=method, timeout=timeout)


def decode_rgba_png(data: bytes) -> tuple[int, int, bytes]:
    """Decode the PNG filters needed to verify output pixels without Pillow."""
    if not data.startswith(PNG_SIGNATURE) or len(data) > 20 * 1024 * 1024:
        raise AssertionError("response is not a bounded PNG")

    offset = len(PNG_SIGNATURE)
    chunks: list[tuple[bytes, bytes]] = []
    while offset < len(data):
        if offset + 12 > len(data):
            raise AssertionError("truncated PNG chunk")
        size = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4 : offset + 8]
        end = offset + 8 + size
        if end + 4 > len(data):
            raise AssertionError("PNG chunk exceeds response length")
        payload = data[offset + 8 : end]
        expected_crc = struct.unpack_from(">I", data, end)[0]
        if zlib.crc32(kind + payload) & 0xFFFFFFFF != expected_crc:
            raise AssertionError(f"bad CRC in {kind!r} chunk")
        chunks.append((kind, payload))
        offset = end + 4
        if kind == b"IEND":
            break
    if offset != len(data) or not chunks or chunks[-1][0] != b"IEND":
        raise AssertionError("missing or trailing PNG data")

    if chunks[0][0] != b"IHDR" or len(chunks[0][1]) != 13:
        raise AssertionError("invalid PNG header")
    width, height, depth, color, compression, filtering, interlace = struct.unpack(
        ">IIBBBBB", chunks[0][1]
    )
    if (depth, compression, filtering, interlace) != (8, 0, 0, 0) or color not in (
        2,
        6,
    ):
        raise AssertionError("PNG is not non-interlaced 8-bit RGB or RGBA")
    if not (0 < width <= 4096 and 0 < height <= 4096):
        raise AssertionError("PNG dimensions exceed test bounds")

    compressed = b"".join(payload for kind, payload in chunks if kind == b"IDAT")
    channels = 4 if color == 6 else 3
    row_size = width * channels
    raw = zlib.decompress(compressed)
    if len(raw) != height * (row_size + 1):
        raise AssertionError("decoded PNG scanline size is inconsistent")

    pixels = bytearray(height * row_size)
    for y in range(height):
        src_start = y * (row_size + 1)
        filter_type = raw[src_start]
        encoded = raw[src_start + 1 : src_start + 1 + row_size]
        row_start = y * row_size
        prev_start = row_start - row_size
        for x, value in enumerate(encoded):
            left = pixels[row_start + x - channels] if x >= channels else 0
            above = pixels[prev_start + x] if y else 0
            upper_left = pixels[prev_start + x - channels] if y and x >= channels else 0
            if filter_type == 1:
                value = (value + left) & 0xFF
            elif filter_type == 2:
                value = (value + above) & 0xFF
            elif filter_type == 3:
                value = (value + ((left + above) // 2)) & 0xFF
            elif filter_type == 4:
                estimate = left + above - upper_left
                distances = (
                    abs(estimate - left),
                    abs(estimate - above),
                    abs(estimate - upper_left),
                )
                predictor = (
                    left
                    if distances[0] <= distances[1] and distances[0] <= distances[2]
                    else above
                    if distances[1] <= distances[2]
                    else upper_left
                )
                value = (value + predictor) & 0xFF
            elif filter_type != 0:
                raise AssertionError(f"unsupported PNG filter {filter_type}")
            pixels[row_start + x] = value
    if channels == 4:
        return width, height, bytes(pixels)
    rgba = bytearray(width * height * 4)
    for source_index in range(0, len(pixels), 3):
        destination_index = source_index // 3 * 4
        rgba[destination_index : destination_index + 3] = pixels[
            source_index : source_index + 3
        ]
        rgba[destination_index + 3] = 255
    return width, height, bytes(rgba)


def pixel(pixels: bytes, width: int, x: int, y: int) -> tuple[int, int, int, int]:
    index = (y * width + x) * 4
    return tuple(pixels[index : index + 4])  # type: ignore[return-value]


class _ServiceTests(unittest.TestCase):
    port = 0
    name = f"wpe-fixture-tests-{os.getpid()}"

    @classmethod
    def setUpClass(cls) -> None:
        docker("image", "inspect", IMAGE)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            cls.port = probe.getsockname()[1]

        docker(
            "run",
            "--detach",
            "--init",
            "--name",
            cls.name,
            "--read-only",
            "--tmpfs",
            "/tmp:rw,nosuid,size=512m",
            "--cap-drop",
            "ALL",
            "--cap-add",
            "SETUID",
            "--cap-add",
            "SETGID",
            "--security-opt",
            "no-new-privileges:true",
            "--pids-limit",
            "128",
            "--memory",
            "768m",
            "--cpus",
            "2",
            "--network",
            "host",
            "--env",
            "WPE_FIXTURE_MODE=1",
            "--env",
            "HA_URL=http://fixture.local",
            "--env",
            "DASHBOARD_PATH=lovelace/energy",
            "--env",
            "EXPECTED_TEXT=Plug Power",
            "--env",
            f"PORT={cls.port}",
            "--env",
            "FIXTURE_READINESS_ERRORS=1",
            "--env",
            "FIXTURE_VIEW_RECREATE_FAILURES=1",
            "--env",
            "RENDER_DEADLINE_SECONDS=5",
            "--env",
            "DASHBOARD_REUSE_IDLE_SECONDS=2",
            IMAGE,
        )

        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                status, _, _ = request("/readyz", timeout=1)
                if status == 200:
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.2)
        logs = docker("logs", cls.name, check=False).stdout
        raise RuntimeError(f"fixture container did not become ready:\n{logs}")

    @classmethod
    def tearDownClass(cls) -> None:
        state = docker(
            "inspect",
            "--format",
            "{{.State.Status}} exit={{.State.ExitCode}} oom={{.State.OOMKilled}} error={{.State.Error}}",
            cls.name,
            check=False,
        )
        if state.returncode == 0 and not state.stdout.startswith("running"):
            logs = docker("logs", cls.name, check=False)
            print(
                "Fixture container exited unexpectedly:\n"
                f"{state.stdout}{logs.stdout}{logs.stderr}",
                flush=True,
            )
        docker("rm", "--force", cls.name, check=False)

    def test_liveness_and_readiness(self) -> None:
        status, headers, body = request("/healthz")
        self.assertEqual((status, body), (200, b"ok\n"))
        self.assertEqual(headers.get_content_type(), "text/plain")
        status, _, body = request("/readyz")
        self.assertEqual((status, body), (200, b"ready\n"))

    def test_normalized_health_routes_do_not_start_screenshots(self) -> None:
        before = docker("logs", self.name, check=False)
        before_count = (before.stdout + before.stderr).count("Screenshot started:")
        for route, expected in (
            ("/healthz/", b"ok\n"),
            ("/./healthz", b"ok\n"),
            ("/readyz/", b"ready\n"),
            ("/./readyz", b"ready\n"),
        ):
            with self.subTest(route=route):
                status, _, body = request(route)
                self.assertEqual((status, body), (200, expected))
        after = docker("logs", self.name, check=False)
        self.assertEqual(
            (after.stdout + after.stderr).count("Screenshot started:"), before_count
        )

    def test_static_service_paths_do_not_start_screenshots(self) -> None:
        before = docker("logs", self.name, check=False)
        before_count = (before.stdout + before.stderr).count("Screenshot started:")
        for route in ("/favicon.ico", "/robots.txt", "/favicon%2eico"):
            with self.subTest(route=route):
                status, headers, body = request(route)
                self.assertEqual(status, 404, body.decode("utf-8", "replace"))
                self.assertEqual(headers.get_content_type(), "text/plain")
        after = docker("logs", self.name, check=False)
        self.assertEqual(
            (after.stdout + after.stderr).count("Screenshot started:"), before_count
        )

    def test_startup_has_no_glib_critical_messages(self) -> None:
        logs = docker("logs", self.name, check=False)
        self.assertNotIn("CRITICAL", logs.stdout + logs.stderr)

    def test_transient_readiness_evaluation_error_is_retried(self) -> None:
        status, _, body = request(ROUTE + "?wait=0")
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        logs = docker("logs", self.name, check=False)
        self.assertIn(
            "Dashboard readiness evaluation failed temporarily; retrying",
            logs.stdout + logs.stderr,
        )

    def test_second_load_finished_does_not_restart_readiness_polling(self) -> None:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        name = f"wpe-load-finished-test-{os.getpid()}"
        fixture_html = (
            "<!doctype html><html><body>"
            '<div data-fixture-ready="true">Plug Power</div>'
            "<script>setTimeout(()=>{location.href='/healthz'},150)</script>"
            "</body></html>"
        )

        with tempfile.TemporaryDirectory(prefix="wpe-load-finished-") as directory:
            fixture_path = os.path.join(directory, "dashboard.html")
            with open(fixture_path, "w", encoding="utf-8") as fixture_file:
                fixture_file.write(fixture_html)
            os.chmod(fixture_path, 0o444)
            docker(
                "run",
                "--detach",
                "--name",
                name,
                "--read-only",
                "--tmpfs",
                "/tmp:rw,nosuid,size=512m",
                "--cap-drop",
                "ALL",
                "--cap-add",
                "SETUID",
                "--cap-add",
                "SETGID",
                "--security-opt",
                "no-new-privileges:true",
                "--network",
                "host",
                "--mount",
                f"type=bind,src={fixture_path},dst=/opt/wpe-engine/duplicate-load.html,readonly",
                "--env",
                "WPE_FIXTURE_MODE=1",
                "--env",
                f"HA_URL=http://127.0.0.1:{port}",
                "--env",
                f"PORT={port}",
                "--env",
                "DASHBOARD_PATH=lovelace/energy",
                "--env",
                "EXPECTED_TEXT=Plug Power",
                "--env",
                "FIXTURE_HTML_PATH=/opt/wpe-engine/duplicate-load.html",
                "--env",
                "RENDER_DEADLINE_SECONDS=5",
                IMAGE,
            )
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    try:
                        if request_at(port, "/readyz", timeout=1)[0] == 200:
                            break
                    except (OSError, urllib.error.URLError):
                        pass
                    time.sleep(0.2)
                else:
                    logs = docker("logs", name, check=False)
                    self.fail(
                        "duplicate-load fixture did not become ready:\n" + logs.stderr
                    )

                status, _, body = request_at(port, ROUTE + "?wait=1000", timeout=8)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(decode_rgba_png(body)[:2], (1280, 800))
                logs = docker("logs", name, check=False)
                lines = (logs.stdout + logs.stderr).splitlines()
                request_loads = [
                    index
                    for index, line in enumerate(lines)
                    if "Dashboard load finished: request_id=1" in line
                ]
                self.assertEqual(
                    len(request_loads), 2, "fixture must cause a second page load"
                )
                self.assertFalse(
                    any(
                        "Dashboard readiness:" in line
                        for line in lines[request_loads[1] + 1 :]
                    ),
                    "readiness polling restarted after capture was scheduled",
                )
            finally:
                docker("rm", "--force", name, check=False)

    def test_encoded_configured_readiness_route_uses_normalized_path(self) -> None:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        name = f"wpe-encoded-readiness-test-{os.getpid()}"
        docker(
            "run",
            "--detach",
            "--init",
            "--name",
            name,
            "--read-only",
            "--tmpfs",
            "/tmp:rw,nosuid,size=512m",
            "--cap-drop",
            "ALL",
            "--cap-add",
            "SETUID",
            "--cap-add",
            "SETGID",
            "--security-opt",
            "no-new-privileges:true",
            "--network",
            "host",
            "--env",
            "WPE_FIXTURE_MODE=1",
            "--env",
            "HA_URL=http://fixture.local",
            "--env",
            f"PORT={port}",
            "--env",
            "DASHBOARD_PATH=lovelace/my%20dash",
            "--env",
            "EXPECTED_TEXT=Plug Power",
            "--env",
            "FIXTURE_FIRST_DELAY_MS=1200",
            "--env",
            "FIXTURE_DELAY_MS=1200",
            "--env",
            "RENDER_DEADLINE_SECONDS=5",
            IMAGE,
        )
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                try:
                    if request_at(port, "/readyz", timeout=1)[0] == 200:
                        break
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(0.2)
            else:
                logs = docker("logs", name, check=False)
                self.fail("encoded-readiness fixture did not start:\n" + logs.stderr)

            status, _, body = request_at(
                port,
                "/lovelace/my%20dash?viewport=640x480&wait=0&format=png",
                timeout=8,
            )
            self.assertEqual(status, 200, body.decode("utf-8", "replace"))
            width, height, pixels = decode_rgba_png(body)
            self.assertEqual((width, height), (640, 480))
            self.assertEqual(
                pixel(pixels, width, 500, 130),
                (32, 208, 80, 255),
                "encoded readiness route must wait for the delayed dashboard fixture",
            )
        finally:
            docker("rm", "--force", name, check=False)

    def test_sigterm_runs_orderly_shutdown(self) -> None:
        name = f"wpe-signal-test-{os.getpid()}"
        docker(
            "run",
            "--detach",
            "--name",
            name,
            "--read-only",
            "--tmpfs",
            "/tmp:rw,nosuid,size=512m",
            "--cap-drop",
            "ALL",
            "--cap-add",
            "SETUID",
            "--cap-add",
            "SETGID",
            "--security-opt",
            "no-new-privileges:true",
            "--pids-limit",
            "128",
            "--memory",
            "768m",
            "--env",
            "WPE_FIXTURE_MODE=1",
            IMAGE,
        )
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                state = docker(
                    "inspect", "--format", "{{.State.Running}}", name, check=False
                )
                if state.returncode == 0 and state.stdout.strip() == "true":
                    logs = docker("logs", name, check=False)
                    if "Screenshot API ready:" in logs.stdout + logs.stderr:
                        break
                time.sleep(0.1)
            else:
                logs = docker("logs", name, check=False)
                self.fail("signal test container did not become ready:\n" + logs.stderr)

            docker("stop", "--time", "3", name)
            exit_code = docker(
                "inspect", "--format", "{{.State.ExitCode}}", name
            ).stdout.strip()
            self.assertEqual(exit_code, "0")
            logs = docker("logs", name, check=False)
            self.assertIn(
                "Shutdown signal received; releasing renderer resources",
                logs.stdout + logs.stderr,
            )
        finally:
            docker("rm", "--force", name, check=False)

    def test_large_request_body_is_rejected_before_continue(self) -> None:
        for method, status in (("POST", 405), ("GET", 400)):
            with self.subTest(method=method):
                with socket.create_connection(
                    ("127.0.0.1", self.port), timeout=3
                ) as client:
                    client.sendall(
                        f"{method} /lovelace/energy HTTP/1.1\r\n".encode()
                        + b"Host: localhost\r\n"
                        + b"Content-Length: 100000000\r\n"
                        + b"Expect: 100-continue\r\n"
                        + b"Connection: keep-alive\r\n\r\n"
                    )
                    response = client.recv(4096)
                self.assertTrue(
                    response.startswith(f"HTTP/1.1 {status}".encode()), response[:200]
                )
                self.assertNotIn(b"100 Continue", response)
                self.assertIn(b"Connection: close", response)

        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as client:
            client.sendall(
                b"GET /lovelace/energy HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Transfer-Encoding: chunked\r\n"
                b"Expect: 100-continue\r\n"
                b"Connection: keep-alive\r\n\r\n"
            )
            response = client.recv(4096)
        self.assertTrue(response.startswith(b"HTTP/1.1 400"), response[:200])
        self.assertNotIn(b"100 Continue", response)
        self.assertIn(b"Connection: close", response)

    def test_oversized_request_target_and_header_are_rejected(self) -> None:
        payloads = {
            "request target": (
                b"GET /"
                + b"a" * (256 * 1024)
                + b" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
            ),
            "header": (
                b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nX-Large: "
                + b"a" * (256 * 1024)
                + b"\r\nConnection: close\r\n\r\n"
            ),
        }
        for label, payload in payloads.items():
            with self.subTest(input=label):
                with socket.create_connection(
                    ("127.0.0.1", self.port), timeout=3
                ) as client:
                    client.settimeout(3)
                    client.sendall(payload)
                    try:
                        response = client.recv(512)
                    except ConnectionResetError:
                        response = b""
                if response:
                    status_line = response.split(b"\r\n", 1)[0]
                    self.assertRegex(status_line, rb"HTTP/1\.[01] (400|414|431)(?: |$)")

        self.assertEqual(request("/healthz")[0], 200)
        logs = docker("logs", self.name, check=False)
        log_text = logs.stdout + logs.stderr
        for diagnostic in (
            "AddressSanitizer",
            "UndefinedBehaviorSanitizer",
            "runtime error:",
            "CRITICAL",
        ):
            self.assertNotIn(diagnostic, log_text)

    def test_default_mcp_request_is_a_complete_expected_png(self) -> None:
        status, headers, body = request(
            ROUTE + "?viewport=1280x800&zoom=1.0&wait=0&format=png"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        self.assertEqual(body[25], 2, "opaque PNG output should use RGB, not RGBA")
        self.assertEqual(headers.get("Cache-Control"), "no-store")
        self.assertEqual(int(headers["Content-Length"]), len(body))
        width, height, pixels = decode_rgba_png(body)
        self.assertEqual((width, height), (1280, 800))
        self.assertEqual(pixel(pixels, width, 5, 5), (36, 63, 96, 255))
        self.assertEqual(pixel(pixels, width, 50, 130), (208, 32, 48, 255))
        self.assertEqual(pixel(pixels, width, 500, 130), (32, 208, 80, 255))
        self.assertEqual(pixel(pixels, width, 510, 150), (240, 208, 32, 255))

    def test_custom_viewport_and_full_page_capture(self) -> None:
        status, headers, body = request(
            ROUTE + "?viewport=640x480&zoom=1.0&wait=0&format=png"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        self.assertEqual(decode_rgba_png(body)[:2], (640, 480))

        for viewport, zoom in (
            ("64x64", "0.1"),
            ("4096x64", "5.0"),
            ("64x4096", "1.0"),
        ):
            with self.subTest(viewport=viewport, zoom=zoom):
                status, headers, body = request(
                    ROUTE + f"?viewport={viewport}&zoom={zoom}&wait=0&format=png"
                )
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(headers.get_content_type(), "image/png")
                self.assertEqual(
                    decode_rgba_png(body)[:2],
                    tuple(int(dimension) for dimension in viewport.split("x")),
                )

        status, headers, body = request(
            ROUTE + "?viewport=1280xauto&zoom=1.0&wait=0&format=png"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        width, height, pixels = decode_rgba_png(body)
        self.assertEqual((width, height), (1280, 1200))
        self.assertIsNone(headers.get("X-WPE-Screenshot-Truncated"))
        self.assertEqual(pixel(pixels, width, 50, 1000), (32, 208, 80, 255))

        status, headers, body = request(
            "/lovelace/tall?viewport=64xauto&zoom=1.0&wait=0&format=png"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        self.assertEqual(headers.get("X-WPE-Screenshot-Truncated"), "true")
        width, height, pixels = decode_rgba_png(body)
        self.assertEqual((width, height), (64, 4000))
        self.assertEqual(pixel(pixels, width, 50, 3900), (32, 208, 80, 255))

        status, _, body = request(
            "/lovelace/fixed-header?viewport=64xauto&zoom=1.0&wait=0&format=png"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        width, height, pixels = decode_rgba_png(body)
        self.assertEqual((width, height), (64, 4000))
        self.assertEqual(pixel(pixels, width, 5, 5), (36, 63, 96, 255))
        self.assertEqual(
            pixel(pixels, width, 5, 805),
            (18, 52, 86, 255),
            "fixed headers should appear in document flow once, not repeat per tile",
        )

        status, _, body = request(
            "/lovelace/inner-scroll?viewport=64xauto&zoom=1.0&wait=0&format=png"
        )
        self.assertEqual(status, 422, body.decode("utf-8", "replace"))
        self.assertIn(b"vertically scrolling inner containers", body)

        for zoom, expected_height, marker_y, marker_x in (
            # At 0.5 zoom the configured 800 px viewport covers 1600 CSS px,
            # so the full-page capture is at least one viewport tall.
            ("0.5", 800, 500, 30),
            ("2.0", 2400, 1900, 100),
        ):
            with self.subTest(auto_height_zoom=zoom):
                status, _, body = request(
                    "/lovelace/energy?viewport=640xauto&zoom="
                    + zoom
                    + "&wait=0&format=png"
                )
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                width, height, pixels = decode_rgba_png(body)
                self.assertEqual((width, height), (640, expected_height))
                self.assertEqual(
                    pixel(pixels, width, marker_x, marker_y), (32, 208, 80, 255)
                )

        for route in ("rounding-short", "rounding"):
            with self.subTest(full_page_rounding=route):
                status, _, body = request(
                    f"/lovelace/{route}?viewport=64xauto&zoom=0.7&wait=0&format=png"
                )
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                width, height, _ = decode_rgba_png(body)
                self.assertEqual(width, 64)
                self.assertGreaterEqual(height, 800)
                self.assertLessEqual(height, 802)

        status, _, body = request(
            "/lovelace/alignment?viewport=64xauto&zoom=0.7&wait=0&format=png"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        width, height, tiled_pixels = decode_rgba_png(body)
        self.assertEqual((width, height), (64, 4000))
        row_bytes = width * 4
        logs = docker("logs", self.name, check=False)
        log_lines = (logs.stdout + logs.stderr).splitlines()
        start_line = next(
            line
            for line in reversed(log_lines)
            if "Screenshot started:" in line and "route=/lovelace/alignment " in line
        )
        request_id = start_line.split("request_id=", 1)[1].split()[0]
        tile_pattern = re.compile(
            rf"Full-page tile captured: request_id={request_id} "
            r"source_y=(\d+) rows=(\d+) next_offset_y=(\d+)"
        )
        tiles = []
        for line in log_lines:
            match = tile_pattern.search(line)
            if match:
                source_y, rows, next_offset = map(int, match.groups())
                tiles.append((next_offset - rows, source_y, rows))
        self.assertGreater(len(tiles), 1, "fixture must exercise tiled capture")
        for offset, source_y, rows in tiles:
            with self.subTest(fractional_zoom_tile_offset=offset):
                status, _, reference_body = request(
                    f"/lovelace/alignment-ref-{offset}"
                    "?viewport=64x800&zoom=0.7&wait=150&format=png"
                )
                self.assertEqual(status, 200, reference_body.decode("utf-8", "replace"))
                ref_width, ref_height, reference_pixels = decode_rgba_png(
                    reference_body
                )
                self.assertEqual((ref_width, ref_height), (64, 800))
                actual_tile = tiled_pixels[
                    offset * row_bytes : (offset + rows) * row_bytes
                ]
                reference_tile = reference_pixels[
                    source_y * row_bytes : (source_y + rows) * row_bytes
                ]
                self.assertEqual(
                    actual_tile,
                    reference_tile,
                    f"stitched rows at document offset {offset} must match an independent viewport capture cropped at source row {source_y}",
                )

    def test_about_blank_navigation_is_not_misclassified_as_cross_origin(self) -> None:
        status, headers, body = request(
            "/lovelace/about-blank?viewport=64x64&zoom=1.0&wait=500&format=png"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        self.assertEqual(decode_rgba_png(body)[:2], (64, 64))

    def test_mcp_frontend_options_and_output_formats(self) -> None:
        status, headers, body = request(
            ROUTE
            + "?viewport=640x480&zoom=1.25&wait=0&format=png"
            + "&theme=Graphite+E-ink+Light&dark&lang=nl"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        width, height, pixels = decode_rgba_png(body)
        self.assertEqual((width, height), (640, 480))
        self.assertEqual(pixel(pixels, width, 100, 380), (32, 208, 80, 255))
        self.assertEqual(pixel(pixels, width, 270, 380), (32, 208, 80, 255))

        status, _, body = request(
            ROUTE + "?viewport=640x480&zoom=1.0&wait=0&format=png"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        _, _, pixels = decode_rgba_png(body)
        self.assertEqual(
            pixel(pixels, 640, 100, 300),
            (32, 64, 208, 255),
            "frontend options from the preceding request must not leak",
        )
        self.assertEqual(
            pixel(pixels, 640, 220, 300),
            (32, 64, 208, 255),
            "the language override must not persist in frontend storage",
        )

        for query, expected_color, label in (
            ("&theme=Graphite+E-ink+Light", (32, 208, 80, 255), "theme only"),
            ("&dark", (32, 64, 208, 255), "dark only"),
            ("&dark=false", (208, 32, 48, 255), "explicit light mode"),
        ):
            with self.subTest(frontend_override=label):
                status, _, body = request(
                    ROUTE + "?viewport=640x480&zoom=1.0&wait=0&format=png" + query
                )
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                _, _, pixels = decode_rgba_png(body)
                self.assertEqual(pixel(pixels, 640, 100, 300), expected_color)

        for dark_value in ("false", "0", "no"):
            with self.subTest(dark_value=dark_value):
                status, _, body = request(
                    ROUTE
                    + "?viewport=640x480&zoom=1.0&wait=0&format=png"
                    + "&theme=Graphite+E-ink+Light&dark="
                    + dark_value
                    + "&lang=nl"
                )
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                _, _, pixels = decode_rgba_png(body)
                self.assertEqual(
                    pixel(pixels, 640, 100, 300),
                    (208, 32, 48, 255),
                    "an explicit false dark value must not enable dark mode",
                )

        status, _, body = request(
            ROUTE
            + "?viewport=640x480&zoom=1.25&wait=0&format=png"
            + "&theme=Graphite%2BE-ink%2BLight&dark&lang=nl"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        _, _, pixels = decode_rgba_png(body)
        self.assertEqual(pixel(pixels, 640, 100, 380), (208, 32, 48, 255))

        formats = (
            ("jpeg", "image/jpeg", b"\xff\xd8\xff"),
            ("webp", "image/webp", b"RIFF"),
            ("bmp", "image/bmp", b"BM"),
        )
        for image_format, mime_type, signature in formats:
            with self.subTest(format=image_format):
                status, headers, body = request(
                    ROUTE + f"?viewport=640x480&zoom=1.0&wait=0&format={image_format}"
                )
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(headers.get_content_type(), mime_type)
                self.assertTrue(body.startswith(signature))
                if image_format == "webp":
                    self.assertEqual(body[8:12], b"WEBP")

        status, _, body = request("/lovelace/no-ha?viewport=64x64&wait=0&theme=Default")
        self.assertEqual(status, 502, body.decode("utf-8", "replace"))
        self.assertIn(b"options could not be applied", body)

    def test_wait_parameter_allows_deferred_dashboard_paint(self) -> None:
        status, headers, body = request(
            "/lovelace/wait-delay?viewport=640x480&zoom=1.0&wait=1200&format=png"
        )
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        width, height, pixels = decode_rgba_png(body)
        self.assertEqual((width, height), (640, 480))
        self.assertEqual(pixel(pixels, width, 520, 340), (32, 208, 80, 255))

    def test_capture_accepts_a_different_dashboard_and_view_path(self) -> None:
        status, headers, body = request("/laundry-dashboard/laundry?wait=0")
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        self.assertEqual(decode_rgba_png(body)[:2], (1280, 800))

        for route in (
            "/laundry-dashboard/laundry%20room?wait=0",
            "/laundry-dashboard/caf%C3%A9?wait=0",
            "/laundry-dashboard/50%25?wait=0",
            "/laundry-dashboard/plus%2Bmark?wait=0",
            "/laundry-dashboard/foo..bar?wait=0",
        ):
            with self.subTest(route=route):
                status, headers, body = request(route)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(headers.get_content_type(), "image/png")
                self.assertEqual(decode_rgba_png(body)[:2], (1280, 800))

    def test_invalid_query_error_is_stable_across_parameter_order(self) -> None:
        first = request(ROUTE + "?wait=30001&format=gif")
        second = request(ROUTE + "?format=gif&wait=30001")
        self.assertEqual(first[0], second[0])
        self.assertEqual(first[2], second[2])
        self.assertEqual(first[0], 422)

    def test_successive_captures_reuse_the_loaded_dashboard_page(self) -> None:
        images = []
        for _ in range(2):
            status, _, body = request(ROUTE + "?wait=0")
            self.assertEqual(status, 200, body.decode("utf-8", "replace"))
            images.append(body)

        marker_colors = []
        for body in images:
            width, height, pixels = decode_rgba_png(body)
            self.assertEqual((width, height), (1280, 800))
            marker_colors.append(pixel(pixels, width, 750, 150))
        self.assertIn(marker_colors[0], ((18, 58, 188, 255), (188, 50, 16, 255)))
        self.assertEqual(marker_colors[0], marker_colors[1])

        time.sleep(2.1)
        status, _, body = request(ROUTE + "?wait=0")
        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
        width, height, pixels = decode_rgba_png(body)
        self.assertEqual((width, height), (1280, 800))
        self.assertNotEqual(marker_colors[1], pixel(pixels, width, 750, 150))

    def test_page_reuse_survives_same_origin_dashboard_redirect(self) -> None:
        before = docker("logs", self.name, check=False)
        before_loads = (before.stdout + before.stderr).count("Dashboard load finished:")
        for _ in range(2):
            status, _, body = request("/lovelace/redirect-root?wait=0")
            self.assertEqual(status, 200, body.decode("utf-8", "replace"))
            self.assertEqual(decode_rgba_png(body)[:2], (1280, 800))
        after = docker("logs", self.name, check=False)
        self.assertEqual(
            (after.stdout + after.stderr).count("Dashboard load finished:"),
            before_loads + 1,
        )

    def test_bad_paths_and_query_inputs_fail_without_images(self) -> None:
        cases = (
            ("/" + "a" * 257, 400),
            ("/lovelace/%2F%2Fevil", 400),
            (ROUTE + "%00suffix", 400),
            (ROUTE + "?viewport=4097xauto", 422),
            (ROUTE + "?viewport=1280x4097", 422),
            (ROUTE + "?viewport=4096x4096", 413),
            (ROUTE + "?viewport=wide", 400),
            (ROUTE + "?zoom=0.05", 422),
            (ROUTE + "?zoom=5.1", 422),
            (ROUTE + "?format=gif", 422),
            (ROUTE + "?dark=maybe", 422),
            (ROUTE + "?theme=+++", 422),
            (ROUTE + "?lang=%E2%80%83", 422),
            (ROUTE + "?unknown=x", 400),
            (ROUTE + "?wait=0&wait=1", 400),
            (ROUTE + "?wait%00shadow=0", 400),
            (ROUTE + "?wait=0%00shadow", 400),
            (ROUTE + "?wait=%ZZ", 400),
            (ROUTE + "?wait=%", 400),
            (ROUTE + "?wait=%2", 400),
            (ROUTE + "?wait=%g1", 400),
            (ROUTE + "?wait=%C0%AF", 400),
            (ROUTE + "?wait=%ED%A0%80", 400),
            (ROUTE + "?wait=%F4%90%80%80", 400),
            (ROUTE + "?" + "&".join("x" for _ in range(17)), 400),
            (ROUTE + "?x=" + "a" * 4100, 400),
        )
        for path, expected_status in cases:
            with self.subTest(path=path[:100]):
                status, headers, body = request(path)
                self.assertEqual(status, expected_status)
                self.assertNotEqual(headers.get_content_type(), "image/png")
                self.assertLessEqual(len(body), 128)

        for byte_value in range(256):
            with self.subTest(percent_byte=byte_value):
                status, headers, body = request(f"{ROUTE}?x=%{byte_value:02X}")
                self.assertEqual(status, 400)
                self.assertNotEqual(headers.get_content_type(), "image/png")
                self.assertLessEqual(len(body), 128)

        status, headers, _ = request(ROUTE, method="POST")
        self.assertEqual(status, 405)
        self.assertEqual(headers.get("Allow"), "GET")

    def test_randomized_request_targets_do_not_crash_or_bypass_path_validation(
        self,
    ) -> None:
        generator = random.Random(0x5745)
        alphabet = string.ascii_letters + string.digits + "%.-_/"
        for index in range(80):
            if index % 2:
                size = generator.randrange(0, 4300)
                value = "".join(generator.choice(alphabet) for _ in range(size))
                path = "/unexpected?x=" + value
            else:
                depth = generator.randrange(1, 6)
                pieces = [
                    "".join(
                        generator.choice(alphabet)
                        for _ in range(generator.randrange(0, 24))
                    )
                    for _ in range(depth)
                ]
                path = "/" + "/".join(pieces)
            with self.subTest(case=index):
                route = path.split("?", 1)[0]
                raw_route = route.strip("/")
                segments = raw_route.split("/") if raw_route else []
                normalized_segments = [
                    segment for segment in segments if segment not in ("", ".")
                ]
                normalized_route = "/".join(normalized_segments)
                safe_route = (
                    len(normalized_route.encode("utf-8")) <= 256
                    and bool(normalized_segments)
                    and all(segment != ".." for segment in normalized_segments)
                    and not any(
                        bit in normalized_route
                        for bit in ("://", "//", "@", "\\", "?", "#")
                    )
                    and all(ord(character) >= 0x20 for character in normalized_route)
                )
                request_path = (
                    path
                    if "?" in path
                    else "/"
                    + "/".join(
                        quote(segment, safe="") for segment in normalized_segments
                    )
                )
                if (
                    safe_route
                    and "?" not in path
                    and route
                    not in (
                        "/healthz",
                        "/readyz",
                    )
                ):
                    separator = "&" if "?" in path else "?"
                    request_path += f"{separator}wait=0"
                status, headers, body = request(request_path, timeout=5)
                if (
                    safe_route
                    and "?" not in path
                    and route
                    not in (
                        "/healthz",
                        "/readyz",
                    )
                ):
                    self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                    self.assertEqual(headers.get_content_type(), "image/png")
                elif route in ("/healthz", "/readyz"):
                    self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                    self.assertEqual(headers.get_content_type(), "text/plain")
                else:
                    self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                    self.assertNotEqual(headers.get_content_type(), "image/png")
                    self.assertLessEqual(len(body), 128)

        self.assertEqual(request("/healthz")[0], 200)

    def test_second_capture_is_rejected_while_renderer_is_busy(self) -> None:
        result: list[tuple[int, object, bytes]] = []

        def capture() -> None:
            result.append(request(ROUTE + "?wait=1000", timeout=5))

        worker = threading.Thread(target=capture)
        worker.start()
        time.sleep(0.1)
        status, headers, _ = request(ROUTE + "?wait=0")
        self.assertEqual(status, 503)
        self.assertEqual(headers.get("Retry-After"), "1")
        worker.join(timeout=5)
        self.assertFalse(worker.is_alive())
        self.assertEqual(result[0][0], 200)

    def test_health_endpoint_stays_responsive_during_image_encoding(self) -> None:
        result: list[tuple[int, object, bytes]] = []

        def capture() -> None:
            result.append(
                request(
                    ROUTE + "?viewport=4096x3600&zoom=1.0&wait=0&format=webp",
                    timeout=30,
                )
            )

        worker = threading.Thread(target=capture)
        worker.start()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            logs = docker("logs", self.name, check=False)
            if "Screenshot encoding started:" in logs.stdout + logs.stderr:
                break
            time.sleep(0.02)
        else:
            worker.join(timeout=1)
            self.fail("large WebP capture did not reach the encoding worker")

        started = time.monotonic()
        status, _, body = request("/healthz", timeout=2)
        elapsed = time.monotonic() - started
        self.assertEqual((status, body), (200, b"ok\n"))
        self.assertLess(elapsed, 1.0, "health check stalled behind image encoding")
        worker.join(timeout=30)
        self.assertFalse(worker.is_alive(), "large WebP encoder did not finish")
        self.assertEqual(result[0][0], 200, result[0][2][:128])
        self.assertEqual(result[0][1].get_content_type(), "image/webp")
        self.assertTrue(result[0][2].startswith(b"RIFF"))

    def test_disconnected_capture_releases_renderer(self) -> None:
        baseline_status, _, baseline_body = request(ROUTE + "?wait=0")
        self.assertEqual(baseline_status, 200)
        baseline_width, _, baseline_pixels = decode_rgba_png(baseline_body)
        baseline_marker = pixel(baseline_pixels, baseline_width, 750, 150)

        existing_logs = docker("logs", self.name, check=False)
        started_count = (existing_logs.stdout + existing_logs.stderr).count(
            "Screenshot started:"
        )
        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as client:
            client.sendall(
                b"GET /lovelace/energy?wait=30000 HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\n"
                b"Connection: close\r\n\r\n"
            )
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                logs = docker("logs", self.name, check=False)
                if (logs.stdout + logs.stderr).count(
                    "Screenshot started:"
                ) > started_count:
                    break
                time.sleep(0.02)
            else:
                self.fail("the server did not begin the disconnected capture")
            reset_connection(client)

        deadline = time.monotonic() + 1.5
        status = 503
        while time.monotonic() < deadline:
            status, headers, body = request(ROUTE + "?wait=0")
            if status == 200:
                self.assertEqual(headers.get_content_type(), "image/png")
                width, height, pixels = decode_rgba_png(body)
                self.assertEqual((width, height), (1280, 800))
                self.assertEqual(
                    pixel(pixels, width, 750, 150),
                    baseline_marker,
                    "disconnecting a capture should preserve the warm dashboard page",
                )
                logs = docker("logs", self.name, check=False)
                self.assertIn("keeping loaded dashboard", logs.stdout + logs.stderr)
                return
            self.assertEqual(status, 503, body.decode("utf-8", "replace"))
            time.sleep(0.05)

        logs = docker("logs", self.name, check=False)
        self.fail(
            f"disconnected client left the renderer busy: status={status}\n"
            + logs.stdout
            + logs.stderr
        )

    def test_half_closed_request_still_receives_capture(self) -> None:
        before = docker("logs", self.name, check=False)
        before_text = before.stdout + before.stderr
        started_count = before_text.count("Screenshot started:")
        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as client:
            client.sendall(
                b"GET /lovelace/energy?wait=0 HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\n"
                b"Connection: close\r\n\r\n"
            )
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                logs = docker("logs", self.name, check=False)
                if (logs.stdout + logs.stderr).count(
                    "Screenshot started:"
                ) > started_count:
                    break
                time.sleep(0.02)
            else:
                self.fail("the server did not begin the half-closed capture")
            client.shutdown(socket.SHUT_WR)
            response = http.client.HTTPResponse(client)
            response.begin()
            body = response.read()
            self.assertEqual(response.status, 200, body.decode("utf-8", "replace"))
            self.assertEqual(response.getheader("Content-Type"), "image/png")
            self.assertEqual(decode_rgba_png(body)[:2], (1280, 800))

    def test_disconnected_full_page_capture_releases_renderer(self) -> None:
        existing_logs = docker("logs", self.name, check=False)
        existing_text = existing_logs.stdout + existing_logs.stderr
        started_count = existing_text.count("Full-page tile captured:")
        completed_count = existing_text.count("Screenshot complete:")
        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as client:
            client.sendall(
                b"GET /lovelace/tall?viewport=64xauto&zoom=1.0&wait=0&format=png HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\n"
                b"Connection: close\r\n\r\n"
            )
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                logs = docker("logs", self.name, check=False)
                log_text = logs.stdout + logs.stderr
                if log_text.count("Full-page tile captured:") > started_count:
                    reset_connection(client)
                    break
                if log_text.count("Screenshot complete:") > completed_count:
                    self.fail("full-page capture completed before disconnection")
                time.sleep(0.01)
            else:
                self.fail("the full-page capture did not render its first tile")

        deadline = time.monotonic() + 1.5
        status = 503
        while time.monotonic() < deadline:
            status, headers, body = request(
                "/lovelace/tall?viewport=64x800&zoom=1.0&wait=0&format=png"
            )
            if status == 200:
                self.assertEqual(headers.get_content_type(), "image/png")
                width, height, pixels = decode_rgba_png(body)
                self.assertEqual((width, height), (64, 800))
                self.assertEqual(
                    pixel(pixels, width, 50, 100),
                    (18, 52, 86, 255),
                    "the next viewport capture must begin at the top after a tiled capture disconnects",
                )
                logs = docker("logs", self.name, check=False)
                self.assertIn(
                    "Screenshot client disconnected:", logs.stdout + logs.stderr
                )
                return
            self.assertEqual(status, 503, body.decode("utf-8", "replace"))
            self.assertEqual(headers.get("Retry-After"), "1")
            time.sleep(0.05)

        logs = docker("logs", self.name, check=False)
        self.fail(
            f"disconnected full-page capture left renderer busy: status={status}\n"
            + logs.stdout
            + logs.stderr
        )

    def test_deadline_resets_view_and_next_capture_succeeds(self) -> None:
        for attempt in range(3):
            with self.subTest(timeout_attempt=attempt + 1):
                status, _, body = request(ROUTE + "?wait=10000", timeout=8)
                self.assertEqual(status, 504, body.decode("utf-8", "replace"))
                self.assertNotIn(PNG_SIGNATURE, body)

        processes = docker("exec", self.name, "ps", "-eo", "stat,args").stdout
        self.assertNotIn("<defunct>", processes)
        self.assertTrue(
            docker(
                "inspect", "--format", "{{.HostConfig.Init}}", self.name
            ).stdout.strip()
            == "true"
        )
        status, headers, image = request(
            ROUTE + "?viewport=1280x800&zoom=1.0&wait=0&format=png", timeout=8
        )
        self.assertEqual(status, 200, image.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        self.assertEqual(decode_rgba_png(image)[:2], (1280, 800))
        logs = docker("logs", self.name, check=False)
        self.assertIn(
            "WPE renderer is unavailable; attempting to recreate the view",
            logs.stdout + logs.stderr,
        )

    def test_web_process_is_non_root_without_nested_bubblewrap(self) -> None:
        self.assertEqual(
            docker("inspect", "--format", "{{.Config.User}}", IMAGE).stdout.strip(),
            "0:0",
        )
        processes = docker("exec", self.name, "ps", "-eo", "uid,args").stdout
        self.assertNotIn("/usr/bin/bwrap", processes)
        self.assertIn("WPEWebProcess", processes)
        renderer_processes = [
            line
            for line in processes.splitlines()
            if any(
                process in line
                for process in (
                    "wpe-screenshot-engine",
                    "/usr/bin/bwrap",
                    "WPEWebProcess",
                )
            )
        ]
        self.assertTrue(renderer_processes, processes)
        self.assertTrue(
            all(line.lstrip().startswith("10001 ") for line in renderer_processes),
            processes,
        )

    def test_root_owned_home_assistant_options_are_handed_off_safely(self) -> None:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        name = f"wpe-options-test-{os.getpid()}"
        token = "OPTIONS-TOKEN-" + uuid.uuid4().hex
        options = (
            '{"home_assistant_url":"http://127.0.0.1:1",'
            '"home_assistant_token":"' + token + '",'
            '"dashboard_path":"lovelace/energy",'
            '"expected_text":"Energy at a glance",'
            '"render_deadline_seconds":2,'
            '"dashboard_reuse_idle_seconds":300,'
            '"webkit_process_memory_limit_mb":512}'
        )
        startup = (
            "printf '%s' '"
            + options
            + "' > /data/options.json; chmod 0600 /data/options.json; "
            + "exec /usr/local/bin/run.sh"
        )
        docker(
            "run",
            "--detach",
            "--rm",
            "--name",
            name,
            "--env",
            "FIXTURE_DELAY_MS=malformed-fixture-value",
            "--env",
            "FIXTURE_HTML_PATH=/" + "x" * 4096,
            "--read-only",
            "--tmpfs",
            "/data:rw,nosuid,size=1m",
            "--tmpfs",
            "/tmp:rw,nosuid,size=512m",
            "--cap-drop",
            "ALL",
            "--cap-add",
            "SETUID",
            "--cap-add",
            "SETGID",
            "--cap-add",
            "CHOWN",
            "--security-opt",
            "no-new-privileges:true",
            "--pids-limit",
            "128",
            "--memory",
            "768m",
            "--publish",
            f"127.0.0.1:{port}:8080",
            "--entrypoint",
            "/bin/sh",
            IMAGE,
            "-c",
            startup,
        )
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                try:
                    if request_at(port, "/healthz", timeout=1)[0] == 200:
                        break
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(0.2)
            else:
                logs = docker("logs", name, check=False)
                self.fail(
                    "root-owned app options were not accepted:\n"
                    + logs.stdout
                    + logs.stderr
                )

            options_mode = docker(
                "exec", name, "stat", "-c", "%u:%a", "/data/options.json"
            ).stdout.strip()
            self.assertEqual(options_mode, "0:600")
            token_file_state = (
                docker(
                    "exec",
                    "--user",
                    "10001:10001",
                    name,
                    "/bin/sh",
                    "-c",
                    (
                        "for token_file in /tmp/wpe-ha-token.*; do "
                        '[ ! -e "$token_file" ] || exit 1; '
                        "done; echo removed"
                    ),
                )
                .stdout.strip()
                .splitlines()
            )
            self.assertEqual(token_file_state, ["removed"])
            processes = docker("exec", name, "ps", "-eo", "uid,args").stdout
            self.assertNotRegex(processes, r"(?m)^0\s+.*wpe-screenshot-engine")
            process_status = docker("exec", name, "cat", "/proc/1/status").stdout
            effective_caps = next(
                line.split()[1]
                for line in process_status.splitlines()
                if line.startswith("CapEff:")
            )
            self.assertEqual(effective_caps, "0000000000000000")
            status, _, body = request_at(port, ROUTE + "?wait=0", timeout=8)
            self.assertEqual(status, 502, body.decode("utf-8", "replace"))
            renderer_environment = docker(
                "exec",
                name,
                "/bin/sh",
                "-c",
                (
                    "for proc in /proc/[0-9]*; do "
                    'if grep -aq WPEWebProcess "$proc/cmdline" 2>/dev/null; then '
                    "tr '\\000' '\\n' < \"$proc/environ\"; exit 0; "
                    "fi; done; exit 1"
                ),
            ).stdout
            self.assertNotIn("HA_TOKEN_FILE=", renderer_environment)
            self.assertNotIn(token, renderer_environment)
        finally:
            docker("rm", "--force", name, check=False)

    def test_temporary_option_token_is_removed_before_later_config_failure(
        self,
    ) -> None:
        token = "TEMP-TOKEN-" + uuid.uuid4().hex
        options = {
            "home_assistant_url": "http://127.0.0.1:1",
            "home_assistant_token": token,
            "dashboard_path": "../invalid-route",
            "expected_text": "",
            "render_deadline_seconds": 55,
            "dashboard_reuse_idle_seconds": 300,
            "webkit_process_memory_limit_mb": 512,
        }
        with tempfile.TemporaryDirectory(prefix="wpe-token-cleanup-") as directory:
            options_path = os.path.join(directory, "options.json")
            tmp_path = os.path.join(directory, "tmp")
            os.mkdir(tmp_path, 0o777)
            os.chmod(tmp_path, 0o777)
            os.mkdir(os.path.join(tmp_path, "wpe-home"), 0o777)
            os.mkdir(os.path.join(tmp_path, "wpe-cache"), 0o777)
            with open(options_path, "w", encoding="utf-8") as options_file:
                json.dump(options, options_file)
            os.chmod(options_path, 0o444)
            result = docker(
                "run",
                "--rm",
                "--read-only",
                "--mount",
                f"type=bind,src={options_path},dst=/data/options.json,readonly",
                "--mount",
                f"type=bind,src={tmp_path},dst=/tmp",
                "--cap-drop",
                "ALL",
                "--cap-add",
                "SETUID",
                "--cap-add",
                "SETGID",
                "--cap-add",
                "CHOWN",
                "--security-opt",
                "no-new-privileges:true",
                IMAGE,
                check=False,
            )
            cleanup = docker(
                "run",
                "--rm",
                "--user",
                "0:0",
                "--mount",
                f"type=bind,src={tmp_path},dst=/tmp",
                "--entrypoint",
                "/bin/sh",
                IMAGE,
                "-c",
                "chmod -R u+rwX /tmp",
                check=False,
            )
            self.assertEqual(cleanup.returncode, 0, cleanup.stdout + cleanup.stderr)
            self.assertEqual(result.returncode, 78, result.stdout + result.stderr)
            self.assertIn("DASHBOARD_PATH", result.stderr)
            self.assertNotIn(token, result.stdout + result.stderr)
            self.assertFalse(
                any(name.startswith("wpe-ha-token.") for name in os.listdir(tmp_path)),
                "temporary Home Assistant token file was left in /tmp",
            )

    def test_startup_waits_for_supervisor_options_on_initial_launch(self) -> None:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        name = f"wpe-options-mount-test-{os.getpid()}"
        docker(
            "run",
            "--detach",
            "--rm",
            "--name",
            name,
            "--read-only",
            "--tmpfs",
            "/data:rw,nosuid,size=1m",
            "--tmpfs",
            "/tmp:rw,nosuid,size=512m",
            "--cap-drop",
            "ALL",
            "--cap-add",
            "SETUID",
            "--cap-add",
            "SETGID",
            "--cap-add",
            "CHOWN",
            "--security-opt",
            "no-new-privileges:true",
            "--pids-limit",
            "128",
            "--memory",
            "768m",
            "--publish",
            f"127.0.0.1:{port}:8080",
            "--env",
            "WPE_FIXTURE_MODE=1",
            "--env",
            "HA_URL=http://fixture.local",
            IMAGE,
        )
        try:
            deadline = time.monotonic() + 5
            logs = ""
            while time.monotonic() < deadline:
                result = docker("logs", name, check=False)
                logs = result.stdout + result.stderr
                if "waiting up to 10 seconds for Supervisor app options" in logs:
                    break
                time.sleep(0.1)
            else:
                self.fail(f"startup did not wait for app options:\n{logs}")

            options = (
                '{"home_assistant_url":"http://fixture.local",'
                '"home_assistant_token":"fixture-token",'
                '"dashboard_path":"lovelace/energy",'
                '"expected_text":"",'
                '"render_deadline_seconds":2,'
                '"dashboard_reuse_idle_seconds":300,'
                '"webkit_process_memory_limit_mb":512}'
            )
            docker(
                "exec",
                name,
                "/bin/sh",
                "-c",
                "printf '%s' '" + options + "' > /data/options.json; "
                "chmod 0600 /data/options.json",
            )

            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                try:
                    if request_at(port, "/readyz", timeout=1)[0] == 200:
                        break
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(0.2)
            else:
                result = docker("logs", name, check=False)
                self.fail(
                    "app did not start after Supervisor options appeared:\n"
                    + result.stdout
                    + result.stderr
                )

            result = docker("logs", name, check=False)
            logs = result.stdout + result.stderr
            self.assertIn("Supervisor app options loaded", logs)
            self.assertIn("Screenshot API ready", logs)
            self.assertNotIn("startup error:", logs)
            status, headers, body = request_at(port, ROUTE + "?wait=0", timeout=8)
            self.assertEqual(status, 200, body.decode("utf-8", "replace"))
            self.assertEqual(headers.get_content_type(), "image/png")
        finally:
            docker("rm", "--force", name, check=False)

    def test_fixture_temporary_token_is_removed_on_orderly_shutdown(self) -> None:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        name = f"wpe-fixture-token-cleanup-{os.getpid()}"
        with tempfile.TemporaryDirectory(prefix="wpe-token-cleanup-") as directory:
            os.chmod(directory, 0o777)
            token_path = os.path.join(directory, "wpe-ha-token.fixture")
            with open(token_path, "w", encoding="ascii") as token_file:
                token_file.write("fixture-token")
            os.chmod(token_path, 0o400)
            docker(
                "run",
                "--detach",
                "--name",
                name,
                "--read-only",
                "--mount",
                f"type=bind,src={directory},dst=/tmp",
                "--cap-drop",
                "ALL",
                "--cap-add",
                "SETUID",
                "--cap-add",
                "SETGID",
                "--security-opt",
                "no-new-privileges:true",
                "--network",
                "host",
                "--env",
                "WPE_FIXTURE_MODE=1",
                "--env",
                "HA_URL=http://fixture.local",
                "--env",
                f"PORT={port}",
                "--env",
                "HA_TOKEN_FILE=/tmp/wpe-ha-token.fixture",
                IMAGE,
            )
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    try:
                        if request_at(port, "/healthz", timeout=1)[0] == 200:
                            break
                    except (OSError, urllib.error.URLError):
                        pass
                    time.sleep(0.2)
                else:
                    logs = docker("logs", name, check=False)
                    self.fail(
                        "fixture token cleanup app did not start:\n" + logs.stderr
                    )

                docker("stop", "--time", "5", name)
                logs = docker("logs", name, check=False)
                self.assertFalse(os.path.exists(token_path), logs.stdout + logs.stderr)
            finally:
                docker("rm", "--force", name, check=False)
                docker(
                    "run",
                    "--rm",
                    "--user",
                    "0:0",
                    "--mount",
                    f"type=bind,src={directory},dst=/tmp",
                    "--entrypoint",
                    "/bin/sh",
                    IMAGE,
                    "-c",
                    "chmod -R u+rwX /tmp",
                    check=False,
                )

    def test_synthetic_ha_token_is_absent_from_errors_and_logs(self) -> None:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        name = f"wpe-token-test-{os.getpid()}"
        sentinel = "TOKEN-SENTINEL-" + uuid.uuid4().hex

        with tempfile.TemporaryDirectory(prefix="wpe-token-test-") as secret_dir:
            secret_path = os.path.join(secret_dir, "ha_token")
            with open(secret_path, "w", encoding="ascii") as token_file:
                token_file.write(sentinel + "\n")
            os.chmod(secret_path, 0o444)
            docker(
                "run",
                "--detach",
                "--rm",
                "--init",
                "--name",
                name,
                "--read-only",
                "--tmpfs",
                "/tmp:rw,nosuid,size=512m",
                "--cap-drop",
                "ALL",
                "--cap-add",
                "SETUID",
                "--cap-add",
                "SETGID",
                "--cap-add",
                "CHOWN",
                "--security-opt",
                "no-new-privileges:true",
                "--pids-limit",
                "128",
                "--memory",
                "768m",
                "--cpus",
                "2",
                "--publish",
                f"127.0.0.1:{port}:8080",
                "--mount",
                f"type=bind,src={secret_path},dst=/run/secrets/ha_token,readonly",
                "--env",
                "HA_URL=http://127.0.0.1:1",
                "--env",
                "HA_TOKEN_FILE=/run/secrets/ha_token",
                "--env",
                "DASHBOARD_PATH=lovelace/energy",
                "--env",
                "EXPECTED_TEXT=Plug Power",
                "--env",
                "RENDER_DEADLINE_SECONDS=2",
                IMAGE,
            )
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    try:
                        if request_at(port, "/healthz", timeout=1)[0] == 200:
                            break
                    except (OSError, urllib.error.URLError):
                        pass
                    time.sleep(0.2)
                else:
                    logs = docker("logs", name, check=False)
                    self.fail(
                        "token test container did not become ready:\n"
                        + logs.stdout
                        + logs.stderr
                    )

                status, _, body = request_at(port, ROUTE + "?wait=0", timeout=8)
                self.assertEqual(status, 502, body.decode("utf-8", "replace"))
                logs = docker("logs", name, check=False)
                log_text = logs.stdout + logs.stderr
                self.assertNotIn(sentinel, body.decode("utf-8", "replace"))
                self.assertNotIn(sentinel, log_text)
            finally:
                docker("rm", "--force", name, check=False)

    def test_oversized_and_binary_token_files_fail_at_startup(self) -> None:
        invalid_tokens = {
            "oversized": b"x" * (8192 + 1),
            "embedded-nul": b"valid-prefix\x00ignored-suffix",
            "invalid-utf8": b"\xff",
        }
        with tempfile.TemporaryDirectory(prefix="wpe-invalid-token-") as secret_dir:
            for label, contents in invalid_tokens.items():
                with self.subTest(token=label):
                    secret_path = os.path.join(secret_dir, "ha_token")
                    if os.path.exists(secret_path):
                        os.chmod(secret_path, 0o600)
                    with open(secret_path, "wb") as token_file:
                        token_file.write(contents)
                    os.chmod(secret_path, 0o444)
                    result = docker(
                        "run",
                        "--rm",
                        "--user",
                        "10001:10001",
                        "--entrypoint",
                        "/usr/local/bin/wpe-screenshot-engine",
                        "--mount",
                        f"type=bind,src={secret_path},dst=/run/secrets/ha_token,readonly",
                        "--env",
                        "HA_URL=http://127.0.0.1:1",
                        "--env",
                        "HA_TOKEN_FILE=/run/secrets/ha_token",
                        IMAGE,
                        check=False,
                    )
                    self.assertEqual(
                        result.returncode, 78, result.stdout + result.stderr
                    )
                    self.assertIn("configuration error", result.stderr)

    def test_fifo_token_path_fails_without_blocking(self) -> None:
        result = subprocess.run(
            [
                "docker",
                "run",
                "--rm",
                "--user",
                "10001:10001",
                "--entrypoint",
                "/bin/sh",
                "--tmpfs",
                "/tmp:rw,nosuid,size=16m",
                IMAGE,
                "-c",
                (
                    "mkfifo /tmp/ha_token && HA_URL=http://127.0.0.1:1 "
                    "HA_TOKEN_FILE=/tmp/ha_token exec /usr/local/bin/wpe-screenshot-engine"
                ),
            ],
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )
        self.assertEqual(result.returncode, 78, result.stdout + result.stderr)
        self.assertIn("configuration error", result.stderr)

    def test_symlink_token_path_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wpe-symlink-token-") as secret_dir:
            target = os.path.join(secret_dir, "real-token")
            token_path = os.path.join(secret_dir, "ha_token")
            with open(target, "w", encoding="utf-8") as token_file:
                token_file.write("fixture-token")
            os.chmod(target, 0o444)
            os.symlink(target, token_path)
            result = docker(
                "run",
                "--rm",
                "--user",
                "10001:10001",
                "--entrypoint",
                "/usr/local/bin/wpe-screenshot-engine",
                "--mount",
                f"type=bind,src={secret_dir},dst=/run/secrets,readonly",
                "--env",
                "HA_URL=http://127.0.0.1:1",
                "--env",
                "HA_TOKEN_FILE=/run/secrets/ha_token",
                IMAGE,
                check=False,
            )
            self.assertEqual(result.returncode, 78, result.stdout + result.stderr)
            self.assertIn("configuration error", result.stderr)

    def test_oversized_configuration_values_fail_before_startup(self) -> None:
        invalid_values = (
            ("DASHBOARD_PATH", "a" * 257),
            ("EXPECTED_TEXT", "a" * 257),
            ("HA_URL", "http://" + "a" * 8192),
            ("HA_TOKEN_FILE", "/" + "a" * 4096),
            ("FIXTURE_HTML_PATH", "/" + "a" * 4096),
            ("PORT", "9" * 5000),
            ("RENDER_DEADLINE_SECONDS", "0"),
            ("DASHBOARD_REUSE_IDLE_SECONDS", "3601"),
            ("FIXTURE_DELAY_MS", "30001"),
        )
        for key, value in invalid_values:
            with self.subTest(config=key):
                result = docker(
                    "run",
                    "--rm",
                    "--user",
                    "10001:10001",
                    "--entrypoint",
                    "/usr/local/bin/wpe-screenshot-engine",
                    "--env",
                    "WPE_FIXTURE_MODE=1",
                    "--env",
                    f"{key}={value}",
                    IMAGE,
                    check=False,
                )
                self.assertEqual(result.returncode, 78, result.stdout + result.stderr)
                self.assertIn("configuration error", result.stderr)

    def test_unsafe_home_assistant_origins_fail_before_startup(self) -> None:
        invalid_urls = (
            "javascript:alert(1)",
            "file:///etc/passwd",
            "ftp://example.com",
            "http://user:password@example.com",
            "http://example.com/base/path",
            "http://example.com?query=1",
            "http://example.com#fragment",
            "http://example.com:0",
            "http://example.com:65536",
            "http://[::1",
            "http://",
        )
        for url in invalid_urls:
            with self.subTest(url=url):
                result = docker(
                    "run",
                    "--rm",
                    "--user",
                    "10001:10001",
                    "--entrypoint",
                    "/usr/local/bin/wpe-screenshot-engine",
                    "--env",
                    "WPE_FIXTURE_MODE=1",
                    "--env",
                    f"HA_URL={url}",
                    IMAGE,
                    check=False,
                )
                self.assertEqual(result.returncode, 78, result.stdout + result.stderr)
                self.assertIn("configuration error", result.stderr)

    def test_fixture_file_is_bounded_and_fifo_does_not_block(self) -> None:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        name = f"wpe-fixture-size-test-{os.getpid()}"

        with tempfile.TemporaryDirectory(
            prefix="wpe-fixture-size-test-"
        ) as fixture_dir:
            fixture_path = os.path.join(fixture_dir, "dashboard.html")
            with open(fixture_path, "wb") as fixture_file:
                fixture_file.write(b"x" * (1024 * 1024 + 1))
            os.chmod(fixture_path, 0o444)
            docker(
                "run",
                "--detach",
                "--rm",
                "--init",
                "--name",
                name,
                "--read-only",
                "--tmpfs",
                "/tmp:rw,nosuid,size=512m",
                "--cap-drop",
                "ALL",
                "--cap-add",
                "SETUID",
                "--cap-add",
                "SETGID",
                "--security-opt",
                "no-new-privileges:true",
                "--pids-limit",
                "128",
                "--memory",
                "768m",
                "--cpus",
                "2",
                "--publish",
                f"127.0.0.1:{port}:8080",
                "--mount",
                f"type=bind,src={fixture_dir},dst=/opt/wpe-engine/test-fixtures,readonly",
                "--env",
                "WPE_FIXTURE_MODE=1",
                "--env",
                "HA_URL=http://fixture.local",
                "--env",
                "DASHBOARD_PATH=lovelace/energy",
                "--env",
                "EXPECTED_TEXT=Plug Power",
                "--env",
                "FIXTURE_HTML_PATH=/opt/wpe-engine/test-fixtures/dashboard.html",
                IMAGE,
            )
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    try:
                        if request_at(port, "/readyz", timeout=1)[0] == 200:
                            break
                    except (OSError, urllib.error.URLError):
                        pass
                    time.sleep(0.2)
                else:
                    self.fail("oversized-fixture container did not become ready")

                status, _, body = request_at(port, ROUTE + "?wait=0", timeout=8)
                self.assertEqual(status, 503, body.decode("utf-8", "replace"))
                self.assertIn(b"Fixture renderer is unavailable", body)

                os.unlink(fixture_path)
                with open(fixture_path, "wb") as fixture_file:
                    fixture_file.write(b"<html>invalid-utf8:\xff</html>")
                status, _, body = request_at(port, ROUTE + "?wait=0", timeout=3)
                self.assertEqual(status, 503, body.decode("utf-8", "replace"))
                self.assertIn(b"Fixture renderer is unavailable", body)

                os.unlink(fixture_path)
                backing_path = os.path.join(fixture_dir, "backing.html")
                with open(backing_path, "w", encoding="utf-8") as fixture_file:
                    fixture_file.write(
                        '<html><body><div data-fixture-ready="true">Plug Power</div></body></html>'
                    )
                os.chmod(backing_path, 0o444)
                os.symlink(backing_path, fixture_path)
                status, _, body = request_at(port, ROUTE + "?wait=0", timeout=3)
                self.assertEqual(status, 503, body.decode("utf-8", "replace"))
                self.assertIn(b"Fixture renderer is unavailable", body)

                os.unlink(fixture_path)
                os.mkfifo(fixture_path, 0o444)
                status, _, body = request_at(port, ROUTE + "?wait=0", timeout=3)
                self.assertEqual(status, 503, body.decode("utf-8", "replace"))
                self.assertIn(b"Fixture renderer is unavailable", body)
            finally:
                docker("rm", "--force", name, check=False)

    def test_cross_origin_frame_navigations_are_blocked_before_request(
        self,
    ) -> None:
        external_requests: list[str] = []

        class ExternalHandler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                external_requests.append(self.path)
                response = (
                    b"<!doctype html><html><body>"
                    b'<div data-fixture-ready="true">External fixture</div>'
                    b"</body></html>"
                )
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

            def log_message(self, format: str, *args: object) -> None:
                del format, args

        external_server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), ExternalHandler
        )
        external_port = external_server.server_address[1]
        external_thread = threading.Thread(
            target=external_server.serve_forever, daemon=True
        )
        external_thread.start()
        self.addCleanup(external_thread.join, 2)
        self.addCleanup(external_server.server_close)
        self.addCleanup(external_server.shutdown)

        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        name = f"wpe-navigation-test-{os.getpid()}"
        html = (
            "<!doctype html><html><body>"
            '<div data-fixture-ready="true">Plug Power</div>'
            "<script>if(location.pathname.endsWith('/iframe')){"
            f"document.write('<iframe src=\"http://127.0.0.1:{external_port}/iframe\"></iframe>');"
            "}</script>"
            "<script>if(location.pathname.endsWith('/blocked'))"
            f"setTimeout(()=>location.href='http://127.0.0.1:{external_port}/main',25);</script>"
            "</body></html>"
        )

        with tempfile.TemporaryDirectory(prefix="wpe-navigation-test-") as fixture_dir:
            fixture_path = os.path.join(fixture_dir, "dashboard.html")
            with open(fixture_path, "w", encoding="utf-8") as fixture_file:
                fixture_file.write(html)
            os.chmod(fixture_path, 0o444)
            docker(
                "run",
                "--detach",
                "--init",
                "--name",
                name,
                "--read-only",
                "--tmpfs",
                "/tmp:rw,nosuid,size=512m",
                "--cap-drop",
                "ALL",
                "--cap-add",
                "SETUID",
                "--cap-add",
                "SETGID",
                "--security-opt",
                "no-new-privileges:true",
                "--pids-limit",
                "128",
                "--memory",
                "768m",
                "--cpus",
                "2",
                "--network",
                "host",
                "--mount",
                f"type=bind,src={fixture_path},dst=/opt/wpe-engine/blocked-navigation.html,readonly",
                "--env",
                "WPE_FIXTURE_MODE=1",
                "--env",
                "HA_URL=http://fixture.local",
                "--env",
                "DASHBOARD_PATH=lovelace/energy",
                "--env",
                "EXPECTED_TEXT=Plug Power",
                "--env",
                f"PORT={port}",
                "--env",
                "FIXTURE_HTML_PATH=/opt/wpe-engine/blocked-navigation.html",
                "--env",
                "RENDER_DEADLINE_SECONDS=2",
                IMAGE,
            )
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    try:
                        if request_at(port, "/healthz", timeout=1)[0] == 200:
                            break
                    except (OSError, urllib.error.URLError):
                        pass
                    time.sleep(0.2)
                else:
                    self.fail("navigation policy container did not become ready")

                status, _, body = request_at(port, "/lovelace/iframe?wait=0", timeout=8)
                self.assertEqual(status, 502, body.decode("utf-8", "replace"))
                self.assertNotIn("/iframe", external_requests)

                try:
                    status, _, body = request_at(
                        port, "/lovelace/blocked?wait=1500", timeout=8
                    )
                except OSError as error:
                    logs = docker("logs", name, check=False)
                    self.fail(
                        f"renderer dropped the blocked-navigation response: {error}\n"
                        + logs.stdout
                        + logs.stderr
                    )
                logs = docker("logs", name, check=False)
                self.assertEqual(
                    status,
                    502,
                    "external main-frame navigation was not blocked; logs:\n"
                    + logs.stdout
                    + logs.stderr
                    + f"\nExternal requests: {external_requests}",
                )
                self.assertIn(b"outside the configured origin was blocked", body)
                self.assertNotIn("/main", external_requests)
                status, _, body = request_at(port, ROUTE + "?wait=0", timeout=8)
                self.assertEqual(
                    status,
                    200,
                    "a blocked redirect should reset the WebKit view for the next request: "
                    + body.decode("utf-8", "replace"),
                )
                logs = docker("logs", name, check=False)
                self.assertNotIn("CRITICAL", logs.stdout + logs.stderr)
            finally:
                docker("rm", "--force", name, check=False)

    def test_private_ca_and_non_panel_route_render(self) -> None:
        invalid_ca = docker(
            "run",
            "--rm",
            "--env",
            "WPE_FIXTURE_MODE=1",
            "--env",
            "HA_CA_CERT_FILE=/ssl/../etc/passwd",
            "--tmpfs",
            "/ssl:rw,nosuid,size=1m",
            IMAGE,
            check=False,
        )
        self.assertEqual(
            invalid_ca.returncode, 78, invalid_ca.stdout + invalid_ca.stderr
        )
        self.assertIn("must resolve inside /ssl", invalid_ca.stderr)

        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            engine_port = probe.getsockname()[1]
        with tempfile.TemporaryDirectory(prefix="wpe-private-ca-") as cert_dir:
            ca_path = os.path.join(cert_dir, "home-assistant-ca.crt")
            key_path = os.path.join(cert_dir, "home-assistant-ca.key")
            docker(
                "run",
                "--rm",
                "--user",
                f"{os.getuid()}:{os.getgid()}",
                "--volume",
                f"{cert_dir}:/ssl",
                "--entrypoint",
                "openssl",
                IMAGE,
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-days",
                "1",
                "-subj",
                "/CN=localhost",
                "-addext",
                "subjectAltName=DNS:localhost,IP:127.0.0.1",
                "-keyout",
                "/ssl/home-assistant-ca.key",
                "-out",
                "/ssl/home-assistant-ca.crt",
            )
            self.assertTrue(os.path.isfile(ca_path))

            class DashboardHandler(http.server.BaseHTTPRequestHandler):
                def do_GET(self) -> None:
                    body = (
                        b"<!doctype html><html><body style='background:#123456'>"
                        b"<h1>Private CA internal page ready</h1>"
                        b"</body></html>"
                    )
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)

                def log_message(self, format: str, *args: object) -> None:
                    return

            tls_server = http.server.ThreadingHTTPServer(
                ("127.0.0.1", 0), DashboardHandler
            )
            tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls_context.load_cert_chain(ca_path, key_path)
            tls_server.socket = tls_context.wrap_socket(
                tls_server.socket, server_side=True
            )
            server_thread = threading.Thread(target=tls_server.serve_forever)
            server_thread.start()

            name = f"wpe-private-ca-test-{os.getpid()}"
            token_path = os.path.join(cert_dir, "ha_token")
            with open(token_path, "w", encoding="ascii") as token_file:
                token_file.write("private-ca-test-token\n")
            os.chmod(token_path, 0o444)
            try:
                docker(
                    "run",
                    "--detach",
                    "--rm",
                    "--name",
                    name,
                    "--network",
                    "host",
                    "--cap-drop",
                    "ALL",
                    "--cap-add",
                    "SETUID",
                    "--cap-add",
                    "SETGID",
                    "--cap-add",
                    "CHOWN",
                    "--security-opt",
                    "no-new-privileges:true",
                    "--pids-limit",
                    "128",
                    "--memory",
                    "768m",
                    "--env",
                    f"PORT={engine_port}",
                    "--env",
                    f"HA_URL=https://localhost:{tls_server.server_port}",
                    "--env",
                    "DASHBOARD_PATH=api/config",
                    "--env",
                    "EXPECTED_TEXT=Private CA internal page ready",
                    "--env",
                    "RENDER_DEADLINE_SECONDS=10",
                    "--env",
                    "HA_TOKEN_FILE=/run/secrets/ha_token",
                    "--env",
                    "HA_CA_CERT_FILE=/ssl/home-assistant-ca.crt",
                    "--mount",
                    f"type=bind,src={ca_path},dst=/ssl/home-assistant-ca.crt,readonly",
                    "--mount",
                    f"type=bind,src={token_path},dst=/run/secrets/ha_token,readonly",
                    IMAGE,
                )
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    try:
                        if request_at(engine_port, "/readyz", timeout=1)[0] == 200:
                            break
                    except (OSError, urllib.error.URLError):
                        pass
                    time.sleep(0.2)
                else:
                    logs = docker("logs", name, check=False)
                    self.fail(
                        "private-CA renderer failed to start:\n"
                        + logs.stdout
                        + logs.stderr
                    )

                status, headers, body = request_at(
                    engine_port,
                    "/api/config?viewport=640x480&zoom=1.0&wait=0&format=png",
                    timeout=15,
                )
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(headers.get_content_type(), "image/png")
                self.assertEqual(decode_rgba_png(body)[:2], (640, 480))
                logs = docker("logs", name, check=False)
                self.assertIn(
                    "custom Home Assistant CA certificate installed",
                    logs.stdout + logs.stderr,
                )
                self.assertIn("state=ready", logs.stdout + logs.stderr)
            finally:
                docker("rm", "--force", name, check=False)
                tls_server.shutdown()
                tls_server.server_close()
                server_thread.join(timeout=3)


if __name__ == "__main__":
    unittest.main()
