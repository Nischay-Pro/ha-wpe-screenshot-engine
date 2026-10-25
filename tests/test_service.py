"""Container-level checks for the WPE screenshot engine's public HTTP surface.

Run from the repository root after building wpe_screenshot_engine/Dockerfile:
    python3 -m unittest discover -s tests

The tests use only a local deterministic fixture and never need an HA token.
"""

from __future__ import annotations

import os
import random
import socket
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

IMAGE = os.environ.get("WPE_IMAGE", "wpe-screenshot-engine:0.1.4")
ROUTE = "/lovelace/energy"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
# These test-only containers run WebKit's nested Bubblewrap sandbox. Docker's
# default seccomp, AppArmor, and masked /proc paths prevent its namespace setup.
RENDERER_SECURITY_OPTIONS = (
    "--security-opt",
    "seccomp=unconfined",
    "--security-opt",
    "apparmor=unconfined",
    "--security-opt",
    "systempaths=unconfined",
)


def docker(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["docker", *args], text=True, capture_output=True, check=check
    )


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
    if (depth, color, compression, filtering, interlace) != (8, 6, 0, 0, 0):
        raise AssertionError("PNG is not non-interlaced 8-bit RGBA")
    if not (0 < width <= 4096 and 0 < height <= 4096):
        raise AssertionError("PNG dimensions exceed test bounds")

    compressed = b"".join(payload for kind, payload in chunks if kind == b"IDAT")
    row_size = width * 4
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
            left = pixels[row_start + x - 4] if x >= 4 else 0
            above = pixels[prev_start + x] if y else 0
            upper_left = pixels[prev_start + x - 4] if y and x >= 4 else 0
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
    return width, height, bytes(pixels)


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
            *RENDERER_SECURITY_OPTIONS,
            "--security-opt",
            "no-new-privileges:true",
            "--pids-limit",
            "128",
            "--memory",
            "768m",
            "--cpus",
            "2",
            "--publish",
            f"127.0.0.1:{cls.port}:8080",
            "--env",
            "WPE_FIXTURE_MODE=1",
            "--env",
            "HA_URL=http://fixture.local",
            "--env",
            "DASHBOARD_PATH=lovelace/energy",
            "--env",
            "EXPECTED_TEXT=Plug Power",
            "--env",
            "RENDER_DEADLINE_SECONDS=2",
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

    def test_startup_has_no_glib_critical_messages(self) -> None:
        logs = docker("logs", self.name, check=False)
        self.assertNotIn("CRITICAL", logs.stdout + logs.stderr)

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
                    response = client.recv(512)
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
        self.assertEqual(headers.get("Cache-Control"), "no-store")
        self.assertEqual(int(headers["Content-Length"]), len(body))
        width, height, pixels = decode_rgba_png(body)
        self.assertEqual((width, height), (1280, 800))
        self.assertEqual(pixel(pixels, width, 5, 5), (36, 63, 96, 255))
        self.assertEqual(pixel(pixels, width, 50, 130), (208, 32, 48, 255))
        self.assertEqual(pixel(pixels, width, 500, 130), (32, 208, 80, 255))
        self.assertEqual(pixel(pixels, width, 510, 150), (240, 208, 32, 255))

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

    def test_bad_paths_and_query_inputs_fail_without_images(self) -> None:
        cases = (
            ("/other", 403),
            ("/" + "a" * 257, 400),
            ("/lovelace/%2e%2e/energy", 403),
            (ROUTE + "%00suffix", 400),
            (ROUTE + "?viewport=4096xauto", 422),
            (ROUTE + "?format=jpeg", 422),
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

    def test_randomized_request_targets_do_not_crash_or_return_images(self) -> None:
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
                status, headers, body = request(path, timeout=5)
                self.assertIn(status, (400, 403))
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

    def test_deadline_resets_view_and_next_capture_succeeds(self) -> None:
        for attempt in range(3):
            with self.subTest(timeout_attempt=attempt + 1):
                status, _, body = request(ROUTE + "?wait=5000", timeout=5)
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
            ROUTE + "?viewport=1280x800&zoom=1.0&wait=0&format=png", timeout=5
        )
        self.assertEqual(status, 200, image.decode("utf-8", "replace"))
        self.assertEqual(headers.get_content_type(), "image/png")
        self.assertEqual(decode_rgba_png(image)[:2], (1280, 800))

    def test_web_process_is_non_root_and_uses_bubblewrap(self) -> None:
        self.assertEqual(
            docker("inspect", "--format", "{{.Config.User}}", IMAGE).stdout.strip(),
            "0:0",
        )
        processes = docker("exec", self.name, "ps", "-eo", "uid,args").stdout
        self.assertIn("/usr/bin/bwrap", processes)
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
            *RENDERER_SECURITY_OPTIONS,
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
            token_handoff = (
                docker(
                    "exec",
                    "--user",
                    "10001:10001",
                    name,
                    "/bin/sh",
                    "-c",
                    (
                        "set -- /tmp/wpe-ha-token.*; "
                        '[ "$#" -eq 1 ] || exit 1; '
                        '[ -r "$1" ] || exit 2; '
                        'stat -c \'%u:%g:%a\' "$1"; cat "$1"'
                    ),
                )
                .stdout.strip()
                .splitlines()
            )
            self.assertEqual(token_handoff, ["10001:10001:400", token])
            processes = docker("exec", name, "ps", "-eo", "uid,args").stdout
            self.assertNotRegex(processes, r"(?m)^0\s+.*wpe-screenshot-engine")
            process_status = docker("exec", name, "cat", "/proc/1/status").stdout
            effective_caps = next(
                line.split()[1]
                for line in process_status.splitlines()
                if line.startswith("CapEff:")
            )
            self.assertEqual(effective_caps, "0000000000000000")
        finally:
            docker("rm", "--force", name, check=False)

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
                *RENDERER_SECURITY_OPTIONS,
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
                    self.fail("token test container did not become ready")

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
                *RENDERER_SECURITY_OPTIONS,
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

    def test_disallowed_frame_navigation_is_blocked_by_policy(self) -> None:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        name = f"wpe-navigation-test-{os.getpid()}"
        html = (
            "<!doctype html><html><body>"
            '<div data-fixture-ready="true">Plug Power</div>'
            "<script>location.href='http://example.invalid/';</script>"
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
                *RENDERER_SECURITY_OPTIONS,
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

                status, _, body = request_at(port, ROUTE + "?wait=0", timeout=8)
                self.assertEqual(status, 502, body.decode("utf-8", "replace"))
                self.assertIn(
                    b"navigation outside the allowed dashboard was blocked", body
                )
            finally:
                docker("rm", "--force", name, check=False)


if __name__ == "__main__":
    unittest.main()
