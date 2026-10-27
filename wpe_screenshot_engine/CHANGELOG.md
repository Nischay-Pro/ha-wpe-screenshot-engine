# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.0] - 2026-10-26

### Added

- Add an optional inbound client IP allowlist supporting IPv4, IPv6, and CIDR ranges.
- Configure the maximum full-page screenshot height and the delay between capture tiles.
- Configure the timeout for applying language, theme, and dark-mode overrides.
- Add `/capabilities` to report supported formats, renderer state, settings, and the effective height limit for a requested width.

### Changed

- Listen on all available IPv4 and IPv6 interfaces by default.
- Stream full-page PNG tiles into one bounded encoder to reduce peak memory use.
- Capture full-page dashboards as stitched viewport tiles and allow time for lazy-loaded cards to paint.

### Fixed

- Return an explicit HTTP 413 error for full-page requests that exceed configured height or pixel limits instead of returning a clipped screenshot.
- Apply defaults for the new settings when upgrading from existing Home Assistant App options.
- Complete full-page captures when zoom rounding leaves a few unreachable rows by repeating the final captured row; reject larger non-scrollable gaps with HTTP 422.
- Keep tile scroll requests at or before the requested output row to avoid skipped rows at fractional zoom.
- Keep screenshot request and Soup object release on the main thread after background encoding.
- Clear expired capture and readiness source IDs before their callbacks are removed.
- Compare requested and reported language codes without case or separator differences.
- Validate screenshot query options before returning the renderer-busy response.
- Remove WebKit's token bootstrap script from the content manager after the first successful readiness check, and log that limited scope accurately.
- Retry once after Home Assistant redirects to sign-in before capture begins, restoring the token bootstrap script.
- Ignore delayed callbacks from replaced views; fail redirects after capture starts instead of reusing partial tile or encoder state.
- Fail unsupported language overrides with HTTP 422 after a short apply window instead of waiting for the render deadline.
- Accept Home Assistant's built-in `default` theme even when it is absent from the custom theme list.
- Return HTTP 504 when Home Assistant theme data does not load before the frontend-options timeout; keep HTTP 422 for unavailable themes and overrides that are not applied.
- Keep language and theme overrides out of Home Assistant profile storage even when frontend persistence runs asynchronously.
- Clarify that cross-origin and opaque document-frame navigations are blocked by policy.
- Allow Compose token paths that resolve to readable regular files, including symlink-based secret mounts.

## [0.2.2] - 2026-10-25

### Added

- Add AVX2 and NEON pixel conversion paths with a scalar fallback.

### Changed

- Accelerate opaque screenshot conversion, white compositing, and PNG opacity checks; use SIMD row conversion for BMP.
- Reduce readiness polling delay for reused dashboard pages.
- Retry failed renderer recreation from `/readyz` while keeping idle-released renderers ready.

### Fixed

- Safely discard canceled encoder results without dereferencing a null task result.
- Limit renderer-unavailable warnings to one per recovery attempt sequence.

## [0.2.1] - 2026-10-25

### Added

- Add a Docker Compose sidecar setup using the prebuilt multi-architecture image and a file-backed Home Assistant token.

### Changed

- Terminate the WebKit web process after the configured idle timeout to reduce idle memory, and recreate it on demand for the next screenshot.
- Use `/healthz` for the container health check so periodic health probes do not wake the renderer.
- Document both Home Assistant App and Docker Compose installation paths, token setup, and MCP network configuration.

## [0.2.0] - 2026-10-25

### Added

- MCP-compatible custom viewport and full-page captures, zoom and wait controls, theme, dark-mode and language options, and JPEG, WebP, and BMP output alongside PNG.
- Optional private CA certificate support for Home Assistant HTTPS connections.

### Changed

- Increase the maximum configurable render deadline from 55 to 300 seconds.
- Preserve Home Assistant's theme or dark-mode setting when the corresponding screenshot option is omitted.
- Keep theme, dark-mode, and language overrides temporary so screenshot requests do not change saved Home Assistant frontend preferences.
- Distinguish a request-side TCP half-close from a full client disconnect so clients can still read the screenshot response.
- Encode opaque PNG captures as RGB in a single bounded compression pass.
- Reduce the raw screenshot buffer limit to 60 MiB and reject oversized fixed viewports before capture.

### Fixed

- Clamp fractional-zoom full-page captures to the last reachable page row.
- Reset the view after a client disconnects during full-page capture so later screenshots start at the top.
- Recreate WebKit views after signal callbacks return and reset the view after cross-origin navigation.
- Ignore transient `about:blank` main-frame notifications.
- Keep a valid loaded dashboard after a client disconnects.
- Allow fully loaded non-panel Home Assistant routes to pass readiness checks.
- Normalize encoded dashboard paths before matching readiness checks.
- Return 404 for root-level `/favicon.ico` and `/robots.txt` requests instead of rendering them as dashboards.
- Reduce readiness polling from 50 ms to 200 ms.
- Ignore fixture-only environment variables outside fixture mode.
- Apply frontend options only after Home Assistant is ready, verify them before capture, and avoid changing saved user preferences.
- Keep health checks responsive while PNG, JPEG, WebP, or BMP images are encoded in a worker thread.
- Bound encoded image memory, transfer response buffers without an extra copy, and remove redundant WebP pixel copies.
- Block cross-origin main-frame and frame navigations before they start.
- Normalize health-route aliases, preserve same-origin page reuse across redirects, and return stable validation errors.
- Report full-page clipping, reject unsupported inner scrolling, and avoid repeating fixed or sticky elements across tiles.
- Remove temporary Home Assistant token files after reading and on orderly fixture shutdown.
- Check headless display creation and return a clear startup failure if WPE cannot initialize.

## [0.1.7] - 2026-10-25

### Fixed

- Make the readiness text optional. Leaving it blank skips the text check and no longer marks the field as required in Home Assistant.

## [0.1.6] - 2026-10-25

### Fixed

- Cancel rendering when a screenshot client disconnects and reset WebKit so later captures are not blocked.
- Wait briefly for Supervisor app options during first startup so a fresh installation can initialize correctly.
- Skip recursive page and shadow-DOM text scans when readiness text is blank.

### Changed

- Accept safe relative screenshot routes on the configured Home Assistant origin; the readiness route is not a route allowlist.
- Improve request lifecycle logging.
- Publish images only after test and lint workflows pass for the same revision.
- Update GitHub Actions to current released versions.

## [0.1.5] - 2026-10-25

### Fixed

- Fix screenshot rendering in Home Assistant app containers where WebKit's nested Bubblewrap sandbox cannot start. The renderer runs as UID `10001` with no Linux capabilities under Supervisor's outer container restrictions.

### Changed

- Map host port `19433` to the app's internal port by default; the host port can be changed in the app's Network settings.
- Remove the temporary Home Assistant token file after startup reads it and before WebKit starts.

## [0.1.4] - 2026-10-25

### Fixed

- Fix startup failure caused by the Home Assistant token file not being readable by the app user.
- Read app settings at startup, securely pass the token to the renderer, and run the renderer as an unprivileged user.

### Changed

- Update the app icon and README.

## [0.1.3] - 2026-10-25

### Added

- First experimental release of WPE Screenshot Engine for Home Assistant.
- Render the configured Home Assistant dashboard route as a 1280 × 800 PNG.
- Support `amd64` and `aarch64` Home Assistant systems.
- Configure the Home Assistant URL and token, dashboard route, readiness text, render deadline, page reuse interval, and WebKit memory target.
- Provide `/healthz` and `/readyz` endpoints.

[0.3.0]: https://github.com/Nischay-Pro/ha-wpe-screenshot-engine/compare/0.2.2...0.3.0
[0.2.2]: https://github.com/Nischay-Pro/ha-wpe-screenshot-engine/compare/0.2.1...0.2.2
[0.2.1]: https://github.com/Nischay-Pro/ha-wpe-screenshot-engine/compare/0.2.0...0.2.1
[0.2.0]: https://github.com/Nischay-Pro/ha-wpe-screenshot-engine/releases/tag/0.2.0
[0.1.7]: https://github.com/Nischay-Pro/ha-wpe-screenshot-engine/releases/tag/0.1.7
[0.1.6]: https://github.com/Nischay-Pro/ha-wpe-screenshot-engine/releases/tag/0.1.6
[0.1.5]: https://github.com/Nischay-Pro/ha-wpe-screenshot-engine/releases/tag/0.1.5
[0.1.4]: https://github.com/Nischay-Pro/ha-wpe-screenshot-engine/releases/tag/0.1.4
[0.1.3]: https://github.com/Nischay-Pro/ha-wpe-screenshot-engine/releases/tag/0.1.3
