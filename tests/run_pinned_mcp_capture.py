"""Call HA MCP's pinned capture function against the local WPE fixture.

The capture.py source is loaded verbatim from the pinned MCP commit. Minimal
stubs replace only the MCP settings/theme-guard imports so the real parameter
builder, HTTPX request, response checks, and capture result run without an HA
instance or credentials.

Required environment:
    HA_MCP_CAPTURE_SOURCE=/path/to/pinned/capture.py
    MCP_ENGINE_URL=http://wpe-fixture:8080
"""

from __future__ import annotations

import asyncio
import hashlib
import importlib.util
import os
import sys
import time
import types
from dataclasses import dataclass
from pathlib import Path
from typing import ClassVar

import httpx

EXPECTED_CAPTURE_SHA256 = (
    "f298cab98dd7615638d699e41a313cc0444c4c7777a4f6b6674d744afe2f206e"
)


def package(name: str) -> types.ModuleType:
    module = types.ModuleType(name)
    module.__path__ = []
    sys.modules[name] = module
    return module


def install_import_stubs(engine_url: str) -> None:
    for name in (
        "ha_mcp",
        "ha_mcp._vendor",
        "ha_mcp._vendor.fastmcp",
        "ha_mcp.tools",
        "ha_mcp.dashboard_screenshot",
    ):
        package(name)

    exceptions = types.ModuleType("ha_mcp._vendor.fastmcp.exceptions")

    class ToolError(Exception):
        pass

    exceptions.ToolError = ToolError
    sys.modules[exceptions.__name__] = exceptions

    errors = types.ModuleType("ha_mcp.errors")

    class ErrorCode:
        pass

    errors.ErrorCode = ErrorCode
    errors.create_error_response = lambda *args, **kwargs: {
        "args": args,
        "kwargs": kwargs,
    }
    sys.modules[errors.__name__] = errors

    helpers = types.ModuleType("ha_mcp.tools.helpers")

    def raise_tool_error(error: object) -> None:
        raise ToolError(str(error))

    helpers.raise_tool_error = raise_tool_error
    sys.modules[helpers.__name__] = helpers

    provision = types.ModuleType("ha_mcp.dashboard_screenshot.provision")
    provision.TOKEN_HINT = "fixture-test"

    @dataclass
    class EngineTarget:
        url: str
        addon_credential: object | None = None

    async def resolve_engine() -> EngineTarget:
        return EngineTarget(engine_url)

    provision.resolve_engine = resolve_engine
    sys.modules[provision.__name__] = provision

    theme_guard = types.ModuleType("ha_mcp.dashboard_screenshot.theme_guard")

    class NoopThemeGuard:
        warnings: ClassVar[list[str]] = []

        async def take_snapshot(self) -> None:
            return None

        async def detect_change(self) -> None:
            return None

    class ThemeGuard:
        @staticmethod
        def for_capture(credential: object | None, client: object | None):
            return NoopThemeGuard()

    theme_guard.ThemeGuard = ThemeGuard
    sys.modules[theme_guard.__name__] = theme_guard


async def main() -> None:
    capture_path = Path(os.environ["HA_MCP_CAPTURE_SOURCE"])
    engine_url = os.environ["MCP_ENGINE_URL"].rstrip("/")
    source_hash = hashlib.sha256(capture_path.read_bytes()).hexdigest()
    if source_hash != EXPECTED_CAPTURE_SHA256:
        raise SystemExit(f"capture.py hash mismatch: {source_hash}")

    install_import_stubs(engine_url)
    sys.path.insert(0, "/tests")
    from test_service import decode_rgba_png, pixel

    async with httpx.AsyncClient(timeout=httpx.Timeout(1.0)) as health_client:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                response = await health_client.get(f"{engine_url}/readyz")
                if response.status_code == 200:
                    break
            except httpx.HTTPError:
                pass
            await asyncio.sleep(0.2)
        else:
            raise RuntimeError("WPE fixture engine did not become ready")

    module_name = "ha_mcp.dashboard_screenshot.capture"
    spec = importlib.util.spec_from_file_location(module_name, capture_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the pinned capture.py module")
    capture = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = capture
    spec.loader.exec_module(capture)

    captures = await capture.capture_dashboard_images("lovelace/energy", wait_ms=0)
    if len(captures) != 1:
        raise AssertionError(f"expected one capture, received {len(captures)}")
    result = captures[0]
    if (result.width, result.height, result.mime_type) != (1280, 800, "image/png"):
        raise AssertionError(
            f"unexpected MCP result metadata: {result.width}x{result.height} "
            f"{result.mime_type}"
        )
    width, height, rgba = decode_rgba_png(result.data)
    if (width, height) != (1280, 800):
        raise AssertionError(f"decoded unexpected dimensions: {width}x{height}")
    if pixel(rgba, width, 500, 130) != (32, 208, 80, 255):
        raise AssertionError("decoded MCP response did not contain fixture pixels")
    print(
        "pinned HA MCP capture passed: "
        f"sha256={source_hash} size={result.size_bytes} bytes "
        f"decoded={width}x{height} mime={result.mime_type}"
    )


if __name__ == "__main__":
    asyncio.run(main())
