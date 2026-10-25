# WPE Screenshot Engine

<p align="center">
  <img src="wpe_screenshot_engine/icon.svg" alt="WPE Screenshot Engine icon" width="64">
</p>

<p align="center">
  Headless WebKit screenshots for Home Assistant dashboards.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/amd64-supported-green" alt="amd64 supported">
  <img src="https://img.shields.io/badge/arm64-supported-green" alt="ARM64 supported">
</p>

<p align="center">
  <a href="https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2FNischay-Pro%2Fha-wpe-screenshot-engine">
    <img src="https://my.home-assistant.io/badges/supervisor_add_addon_repository.svg" alt="Add repository to Home Assistant">
  </a>
</p>

WPE Screenshot Engine renders any requested Home Assistant dashboard route as a 1280 × 800 PNG. It provides a small HTTP API for [Home Assistant MCP](https://github.com/homeassistant-ai/ha-mcp) and other screenshot clients.

## Features

- Renders dashboards with headless WPE WebKit.
- Waits for the requested dashboard to load before returning a screenshot. An optional text check applies only to its configured readiness route.
- Reuses the loaded page between captures to reduce rendering time.
- Supports `amd64` and `arm64` Home Assistant systems with prebuilt images.
- Accepts safe relative paths for dashboards and views on the configured Home Assistant origin.

## Installation

Requires Home Assistant OS or a Supervised installation with the Apps feature.

1. Select **Add repository** above. To add it manually, go to **Settings → Apps → Install app → ⋮ → Repositories** and enter:

   `https://github.com/Nischay-Pro/ha-wpe-screenshot-engine`

2. Install **WPE Screenshot Engine**.
3. Open its **Configuration** tab and enter the required settings.
4. Save the configuration and start the app.

Home Assistant downloads the image for your system architecture when you install or update the app.

## Configuration

The Home Assistant access token is required. The app configuration form masks the token field.

| Setting | Description | Default |
| --- | --- | --- |
| Home Assistant URL | Base HTTP or HTTPS URL reachable from the app, without a path. The default internal hostname is correct for Supervisor apps. | `http://homeassistant:8123` |
| Home Assistant access token | Long-lived token for a dedicated, non-admin Home Assistant user. | Required |
| Readiness check route | Route where the readiness text is checked, if set. It does not restrict which route the screenshot client can request. | `lovelace/energy` |
| Readiness text | Optional visible text required on the readiness check route. Leave blank to disable the text check; other requested routes skip it. | Blank |
| Render deadline | Maximum time allowed for one capture, from 1 to 55 seconds. | `55` seconds |
| Page reuse idle interval | How long to keep the dashboard loaded between captures. Set to `0` to reload for every request. | `300` seconds |
| WebKit memory target | WebKit process memory pressure target, from 512 to 3072 MiB. | `512` MiB |

Create the access token from the dedicated user's Home Assistant profile. Give that user access only to the dashboards it needs to view. The token is stored in the app configuration and passed to the renderer through a private temporary file.

## Connect Home Assistant MCP

1. Enable dashboard screenshots in Home Assistant MCP.
2. Open **WPE Screenshot Engine → Info → Network** and note its published host port. The default mapping is `19433` on the Home Assistant host to `8080` inside the app.
3. In Home Assistant MCP, set the screenshot engine URL to `http://<home-assistant-host>:<published-port>`. Example: `http://192.168.1.2:19433`; replace `192.168.1.2` with your Home Assistant host's address.

Use the app's internal hostname with port `8080` only when the MCP process is on a network that can resolve and reach that hostname. Otherwise, use the Home Assistant host address and published port above. Do not use `localhost` unless MCP and the screenshot engine run in the same network namespace; there it points to MCP itself. If port `19433` is already in use, change the published port in the app's **Network** settings and update the MCP URL to match.

The screenshot client selects the dashboard path on each request. The app's readiness route and text are not an allowlist.

For a Docker-based MCP installation, set `HAMCP_DASHBOARD_SCREENSHOT_ENGINE_URL` to the same reachable URL in the MCP container's environment, then restart that container. Follow the [Home Assistant MCP screenshot guide](https://github.com/homeassistant-ai/ha-mcp/blob/master/docs/beta.md) for the MCP-side setup.

## HTTP API

The request path selects the dashboard and view. For example:

```text
GET http://<app-hostname>:8080/lovelace/energy?viewport=1280x800&zoom=1.0&wait=2500&format=png
```

Supported query parameters:

| Parameter | Supported value | Default |
| --- | --- | --- |
| `viewport` | `1280x800` | — |
| `zoom` | `1.0` | — |
| `wait` | `0` to `30000` milliseconds | `2500` |
| `format` | `png` | — |

The endpoint returns `image/png`. Use any safe relative Home Assistant dashboard path in place of `lovelace/energy`. `GET /healthz` checks whether the service is running; `GET /readyz` checks whether the renderer is ready.

## Network and security

The API listens on port `8080` inside the Home Assistant app network. Host port `19433` is mapped to it by default and can be changed in the app's **Network** settings. The screenshot API has no separate authentication, so keep the mapped port on a trusted network and do not expose it directly to the internet. It validates requested dashboard paths and keeps browser navigation on the configured Home Assistant origin. Use a dedicated, non-admin Home Assistant user for the access token.

Nested Linux namespaces are restricted inside Home Assistant apps, which prevents WebKit's Bubblewrap process sandbox from starting. The app disables that inner sandbox so rendering works in the app container; Supervisor's outer container restrictions remain enabled, and the renderer runs as UID `10001` with no Linux capabilities. This reduces browser-process isolation, so use trusted dashboards. The app removes its temporary token file after reading it and before starting WebKit. WebKit's [upstream sandbox override](https://bugs.webkit.org/show_bug.cgi?id=250232) is deliberately named to flag this security tradeoff.

## Troubleshooting

- **First start reports that no token is available:** set the Home Assistant access token in the app's **Configuration** before starting. Startup waits briefly for Supervisor options to appear and reports a direct configuration error if the token is still missing.
- **Login page or authorization error:** check the Home Assistant URL and token, and confirm the token's user can open the requested dashboard.
- **Dashboard never becomes ready:** the app waits for the Home Assistant frontend to finish loading. If a readiness text is configured, it is checked only on the configured readiness route.
- **MCP cannot connect:** check the URL in the MCP configuration. Use port `8080` only over the shared app network; outside it, use the Home Assistant host address and mapped port (default `19433`).

## Development

Build the app image and run the HTTP and security regression tests on Ubuntu:

```sh
docker build --tag wpe-screenshot-engine:0.1.7 ./wpe_screenshot_engine
python3 -m unittest discover -s tests -v
pre-commit install
pre-commit run --all-files
```

GitHub Actions builds and publishes installable images for `amd64` and `arm64` when a GitHub release is published.
