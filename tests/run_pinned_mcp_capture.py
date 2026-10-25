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

    default_captures = await capture.capture_dashboard_images(
        "lovelace/energy", wait_ms=0
    )
    if len(default_captures) != 1:
        raise AssertionError(
            f"expected one default capture, received {len(default_captures)}"
        )
    default = default_captures[0]
    if (default.width, default.height, default.mime_type) != (
        1280,
        800,
        "image/png",
    ):
        raise AssertionError(
            f"unexpected default MCP result: {default.width}x{default.height} "
            f"{default.mime_type}"
        )
    width, height, rgba = decode_rgba_png(default.data)
    if (width, height) != (1280, 800):
        raise AssertionError(f"decoded unexpected dimensions: {width}x{height}")
    if pixel(rgba, width, 500, 130) != (32, 208, 80, 255):
        raise AssertionError("decoded MCP response did not contain fixture pixels")

    encoded_paths = (
        "laundry-dashboard/laundry room",
        "laundry-dashboard/café",
        "laundry-dashboard/50%",
        "laundry-dashboard/plus+mark",
    )
    for dashboard_path in encoded_paths:
        path_captures = await capture.capture_dashboard_images(
            dashboard_path, wait_ms=0
        )
        if len(path_captures) != 1 or (
            path_captures[0].width,
            path_captures[0].height,
        ) != (1280, 800):
            raise AssertionError(
                f"MCP-encoded dashboard path was not rendered: {dashboard_path!r}"
            )

    full_page_captures = await capture.capture_dashboard_images(
        "lovelace/energy",
        width=1280,
        height="auto",
        wait_ms=0,
        render_timeout_seconds=10,
    )
    if len(full_page_captures) != 1:
        raise AssertionError(
            f"expected one full-page capture, received {len(full_page_captures)}"
        )
    full_page = full_page_captures[0]
    width, height, rgba = decode_rgba_png(full_page.data)
    if (full_page.width, full_page.height) != (1280, "auto") or (
        width,
        height,
    ) != (1280, 1200):
        raise AssertionError(
            f"unexpected full-page capture: {width}x{height}, "
            f"requested metadata={full_page.width}x{full_page.height}"
        )
    if pixel(rgba, width, 50, 1000) != (32, 208, 80, 255):
        raise AssertionError("full-page MCP response omitted below-fold fixture pixels")

    full_page_alias = await capture.capture_dashboard_images(
        "lovelace/energy",
        width=1280,
        full_page=True,
        wait_ms=0,
        render_timeout_seconds=10,
    )
    if len(full_page_alias) != 1:
        raise AssertionError(
            f"expected one full_page alias capture, received {len(full_page_alias)}"
        )
    alias_width, alias_height, alias_rgba = decode_rgba_png(full_page_alias[0].data)
    if (alias_width, alias_height) != (1280, 1200) or pixel(
        alias_rgba, alias_width, 50, 1000
    ) != (32, 208, 80, 255):
        raise AssertionError("full_page alias did not capture the whole fixture page")

    full_page_responsive = await capture.capture_dashboard_images(
        "lovelace/energy",
        viewport_presets=["mobile", "desktop"],
        full_page=True,
        wait_ms=0,
        render_timeout_seconds=300,
    )
    expected_full_page_viewports = ((390, 1200), (1280, 1200))
    if len(full_page_responsive) != len(expected_full_page_viewports):
        raise AssertionError("full-page preset batch returned the wrong image count")
    for result, dimensions in zip(
        full_page_responsive, expected_full_page_viewports, strict=True
    ):
        actual_dimensions = decode_rgba_png(result.data)[:2]
        if actual_dimensions != dimensions:
            raise AssertionError(
                f"unexpected full-page preset dimensions: {actual_dimensions}"
            )

    responsive = await capture.capture_dashboard_images(
        "lovelace/energy",
        viewport_presets=["mobile", "tablet", "desktop"],
        orientation="landscape",
        zoom=1.25,
        wait_ms=0,
        theme="Graphite E-ink Light",
        dark_mode=True,
        language="nl",
        image_format="png",
        render_timeout_seconds=10,
    )
    expected_viewports = ((844, 390), (1024, 768), (1280, 800))
    if len(responsive) != len(expected_viewports):
        raise AssertionError(
            f"expected three responsive captures, received {len(responsive)}"
        )
    for result, dimensions in zip(responsive, expected_viewports, strict=True):
        if (result.width, result.height, result.mime_type) != (
            *dimensions,
            "image/png",
        ):
            raise AssertionError(
                f"unexpected responsive MCP result: {result.width}x{result.height} "
                f"{result.mime_type}"
            )
        actual_width, actual_height, rgba = decode_rgba_png(result.data)
        if (actual_width, actual_height) != dimensions or pixel(
            rgba, actual_width, 100, 380
        ) != (32, 208, 80, 255):
            raise AssertionError(
                "responsive MCP capture did not apply zoom, theme, dark mode, "
                "and language"
            )

    portrait = await capture.capture_dashboard_images(
        "lovelace/energy",
        viewport_presets=["mobile", "tablet", "desktop"],
        orientation="portrait",
        wait_ms=0,
        render_timeout_seconds=10,
    )
    expected_portrait_viewports = ((390, 844), (768, 1024), (800, 1280))
    if len(portrait) != len(expected_portrait_viewports):
        raise AssertionError("portrait preset batch returned the wrong image count")
    for result, dimensions in zip(portrait, expected_portrait_viewports, strict=True):
        actual_dimensions = decode_rgba_png(result.data)[:2]
        if actual_dimensions != dimensions:
            raise AssertionError(
                f"unexpected portrait preset dimensions: {actual_dimensions}"
            )

    formats = (
        ("jpeg", "image/jpeg", b"\xff\xd8\xff"),
        ("webp", "image/webp", b"RIFF"),
        ("bmp", "image/bmp", b"BM"),
    )
    for image_format, mime_type, signature in formats:
        formatted = await capture.capture_dashboard_images(
            "lovelace/energy",
            width=640,
            height=480,
            wait_ms=0,
            image_format=image_format,
            render_timeout_seconds=10,
        )
        if len(formatted) != 1:
            raise AssertionError(
                f"expected one {image_format} capture, received {len(formatted)}"
            )
        result = formatted[0]
        if (result.width, result.height, result.mime_type) != (
            640,
            480,
            mime_type,
        ) or not result.data.startswith(signature):
            raise AssertionError(
                f"unexpected {image_format} MCP result: {result.width}x{result.height} "
                f"{result.mime_type}"
            )
        if image_format == "webp" and result.data[8:12] != b"WEBP":
            raise AssertionError("MCP webp response has an invalid file signature")

    print(
        "pinned HA MCP capture passed: "
        f"sha256={source_hash} default=1280x800 full_page=1280x1200+alias "
        "responsive=landscape+portrait full_page_presets=true "
        "options=zoom,theme,dark,lang "
        "formats=png,webp,jpeg,bmp"
    )


if __name__ == "__main__":
    asyncio.run(main())
