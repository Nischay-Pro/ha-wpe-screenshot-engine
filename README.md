# WPE Screenshot Engine app repository

An experimental Home Assistant app that renders a configured Home Assistant frontend route with headless WPE WebKit and serves a 1280×800 PNG endpoint for [Home Assistant MCP](https://github.com/homeassistant-ai/ha-mcp).

[![Add this repository to Home Assistant](https://my.home-assistant.io/badges/supervisor_add_addon_repository.svg)](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2FNischay-Pro%2Fha-wpe-screenshot-engine)

## Apps

### WPE Screenshot Engine

Renders one configured Home Assistant frontend route as a PNG for screenshot clients. The app reuses the loaded page between captures so live entities can continue updating. It defaults to `lovelace/energy` and accepts other safe relative routes on the configured Home Assistant origin. It is available for `amd64` and Home Assistant's `aarch64` architecture name for ARM64.

## Install

1. In Home Assistant, open **Settings → Apps → Install app**.
2. Open the menu and select **Repositories**.
3. Add `https://github.com/Nischay-Pro/ha-wpe-screenshot-engine`.
4. Install **WPE Screenshot Engine** from the repository.

After tests pass on `main`, GitHub Actions publishes a multi-architecture `edge` image and an immutable commit-specific image to GitHub Container Registry. A nightly run also publishes a dated image and updates the `nightly` tag. These are for testing; they do not change the version installed by Home Assistant. To publish an installable release, create a GitHub release whose tag exactly matches the app version in `wpe_screenshot_engine/config.yaml` (currently `0.1.3`). That publishes the versioned image and `latest` tag. After the first release, make the GHCR package public so Home Assistant can pull it. Home Assistant pulls the image matching the app version and device architecture instead of building it on the HA host.

## Configure and connect

On first install, enter a long-lived Home Assistant access token in the app's masked **Home Assistant access token** field. Use a dedicated, non-admin Home Assistant user. You can use a `!secret` reference instead of entering the token directly. The app writes the resolved value to a mode-restricted file on its temporary filesystem; it is not included in logs.

Set the Home Assistant URL and route path in the app's **Configuration** tab. Change the readiness text to text that appears on the selected page. The page reuse interval and WebKit memory pressure target are also configurable there.

For HA MCP, enable its `ha_get_dashboard_screenshot` beta feature and set `HAMCP_DASHBOARD_SCREENSHOT_ENGINE_URL` to the app's internal hostname on port `8080`. Home Assistant prefixes that hostname with the repository ID, so it differs by repository URL. See the [HA MCP beta guide](https://github.com/homeassistant-ai/ha-mcp/blob/master/docs/beta.md).

The app listens internally on port `8080`. Its host port mapping is disabled by default; users can map it to a custom host port in the app's **Network** settings if needed. The endpoint has no inbound authentication; keep the app and its screenshot client on a trusted Home Assistant app network.

## Development checks

GitHub Actions runs source and configuration linters, the Home Assistant app linter, and Hadolint for the Dockerfile on each push and pull request. To run the source hooks locally, install `pre-commit`, `cppcheck`, `shellcheck`, and `yamllint`, then run `pre-commit run --all-files`.

The test workflow builds the app for `amd64` and `aarch64` on native runners, then runs fixture-based HTTP, input-validation, token-handling, renderer-reuse, and PNG-output checks against each image. It also runs the pinned Home Assistant MCP capture client against the fixture on both architectures. These checks do not need a live Home Assistant instance or token.
