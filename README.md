# Home Assistant App: WPE Screenshot Engine

![Supports aarch64 Architecture](https://img.shields.io/badge/aarch64-yes-green.svg)
![Supports amd64 Architecture](https://img.shields.io/badge/amd64-yes-green.svg)

An experimental Home Assistant app that renders one configured dashboard route as a 1280 × 800 PNG using headless WPE WebKit. It provides an HTTP screenshot endpoint for clients such as [Home Assistant MCP](https://github.com/homeassistant-ai/ha-mcp); it does not have a separate web dashboard.

[![Add repository to Home Assistant](https://my.home-assistant.io/badges/supervisor_add_addon_repository.svg)](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2FNischay-Pro%2Fha-wpe-screenshot-engine)

## Install

This app requires Home Assistant OS or a Supervised installation with the Apps feature.

1. Select the **Add repository** button above. To add it manually, open **Settings → Apps → Install app → ⋮ → Repositories** and enter `https://github.com/Nischay-Pro/ha-wpe-screenshot-engine`.
2. Install **WPE Screenshot Engine**.
3. Configure the app, then start it.

## Configure

Open the app's **Configuration** tab. The access token is required.

| Option | What to enter | Default |
| --- | --- | --- |
| Home Assistant URL | Base URL reachable from this app, with no path. | `http://homeassistant:8123` |
| Home Assistant access token | Long-lived token for a dedicated, non-admin Home Assistant user. The field is masked. | — |
| Home Assistant route | Relative dashboard path, without a leading slash. For example, `lovelace/energy`. | `lovelace/energy` |
| Readiness text | Text that appears on the dashboard before a screenshot is returned. | `Energy at a glance` |
| Render deadline | Maximum time allowed for a screenshot, from 1 to 55 seconds. | `55` |
| Page reuse idle interval | Seconds to keep the dashboard loaded between captures, from 0 to 3600. Set to `0` to reload each time. | `300` |
| WebKit memory target | WebKit process memory pressure target, from 512 to 3072 MiB. | `512` MiB |

Create the token from a dedicated Home Assistant user's profile. Grant only the access that user needs to view the dashboard. The app stores the token in a restricted temporary file and does not write it to its logs.

The app renders the one route configured above. To switch dashboards, change the route and restart the app.

## Connect Home Assistant MCP

1. Enable the dashboard screenshot feature in your Home Assistant MCP settings.
2. Set its screenshot engine URL to `http://<app-hostname>:8080`.

Use the WPE Screenshot Engine's internal Home Assistant app hostname. When both apps are installed through Home Assistant, the hostname may include the repository identifier. Do not use `localhost` from the MCP app; that points back to the MCP app itself. The route comes from the WPE app's **Home Assistant route** setting. For Docker-based MCP deployments, set `HAMCP_DASHBOARD_SCREENSHOT_ENGINE_URL` to the same base URL.

See the [Home Assistant MCP screenshot guide](https://github.com/homeassistant-ai/ha-mcp/blob/master/docs/beta.md) for current feature settings.

## Network and security

The screenshot API listens on port `8080` inside the app network. Home Assistant does not publish a host port by default. If another device needs direct access, map a host port in the app's **Network** settings.

The API does not require a second token. Keep it on a trusted network and do not expose it directly to the internet.

## Troubleshooting

- **Login page or authorization error:** Check the Home Assistant URL and token. Make sure the token's user can open the configured dashboard.
- **Dashboard never becomes ready:** Check that the readiness text is visible on the selected route. Update the route or readiness text if needed.
- **MCP cannot connect:** Confirm the MCP screenshot engine URL uses the WPE app's internal hostname and port `8080`.
- **Check app health:** `GET /healthz` reports whether the service is running; `GET /readyz` reports whether the renderer is ready.

## For maintainers

GitHub Actions tests changes on pushes and pull requests, then publishes `edge` images after successful `main` builds. A nightly run publishes a `nightly` image for testing. To publish an installable app update, increase `version` in `wpe_screenshot_engine/config.yaml`, wait for checks to pass, then publish a GitHub release with the same version as its tag. The build publishes images for `amd64` and `aarch64`; make the GHCR package public after its first publish.

To install the local pre-commit hook, install `pre-commit`, `cppcheck`, `shellcheck`, and `yamllint`, then run `pre-commit install`. Run all hooks manually with `pre-commit run --all-files`.
