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

WPE Screenshot Engine renders Home Assistant dashboards as images for [Home Assistant MCP](https://github.com/homeassistant-ai/ha-mcp) and other clients. It defaults to a 1280 × 800 PNG and supports custom sizes, full-page captures, themes, and image formats. Prebuilt images support `amd64` and `arm64`.

## Before you start: create a Home Assistant token

Create a long-lived access token before installing or configuring the engine:

1. Use a dedicated, non-admin Home Assistant user and give it access to the dashboards you want to capture.
2. In Home Assistant, open that user's profile, go to **Security**, and create a **Long-Lived Access Token**.
3. Copy the token somewhere safe. You will enter it in the Home Assistant App settings or save it in the Compose secret file in the steps below.

## Installation

Choose one installation path, then follow **Connect Home Assistant MCP** below.

> [!WARNING]
> The screenshot API has no inbound authentication. Keep its published port reachable only by trusted clients.

### Home Assistant App (Recommended)

Requires Home Assistant OS or a Supervised installation with the Apps feature.

1. Select **Add repository** above. To add it manually, go to **Settings → Apps → Install app → ⋮ → Repositories** and enter:

   `https://github.com/Nischay-Pro/ha-wpe-screenshot-engine`

2. Install **WPE Screenshot Engine**.
3. Open its **Configuration** tab and set the Home Assistant URL and access token. The URL defaults to `http://homeassistant:8123`, which is correct for Supervisor apps.
4. Save the configuration and start the app.

Home Assistant downloads the image for your system architecture when you install or update the app.

#### App settings

Paste the token created above into **Home Assistant access token**. The form masks it. The Home Assistant URL defaults to `http://homeassistant:8123`.

The other options are optional:

- **Home Assistant CA certificate:** PEM certificate under Home Assistant's `/ssl` directory; leave blank for public or system-trusted certificates.
- **Readiness check route:** defaults to `lovelace/energy`. This checks renderer readiness; it does not restrict screenshot routes.
- **Readiness text:** optional text checked on the readiness route; blank skips the text check.
- **Render deadline:** maximum capture time, 1–300 seconds; default `55` seconds.
- **Renderer idle timeout:** time before WebKit stops to release memory; default `300` seconds. Set to `0` to release it after each request.
- **WebKit memory target:** 512–3072 MiB; default `512` MiB.

For HTTPS with a private CA, place the PEM certificate under Home Assistant's `/ssl` directory and set **Home Assistant CA certificate** to its path, such as `/ssl/home-assistant-ca.crt`.

### Docker Compose (Advanced)

Use this path for a Docker deployment. It runs only the screenshot engine; it does not install Home Assistant or MCP. The [`docker-compose.yml`](docker-compose.yml) and `.env.example` are at the repository root. Run these commands there, not inside `wpe_screenshot_engine/`. The Compose file pulls the prebuilt `latest` image.

1. Clone the repository, copy the environment example, and create the secret directory:

   ```sh
   git clone https://github.com/Nischay-Pro/ha-wpe-screenshot-engine.git
   cd ha-wpe-screenshot-engine
   cp .env.example .env
   mkdir -p secrets
   ```

2. Edit `.env` and set `HA_URL` to the Home Assistant URL reachable from the container. Use a Home Assistant service name only if it is on a shared Docker network; otherwise use a reachable host address.
3. Create `secrets/ha_token` and paste the token created above into it. Compose mounts this file as a Docker secret; the token is not put in the container environment. `.env` and `secrets/` are gitignored.
4. Start the sidecar:

   ```sh
   docker compose -f docker-compose.yml up -d
   ```

The Home Assistant App and Compose have the same screenshot options and defaults. The app stores the token in its masked field and sets the host port in **Info → Network**; Compose uses `secrets/ha_token` and `WPE_HOST_PORT`. The sidecar listens on port `8080` inside Docker and publishes host port `19433` by default. `.env.example` describes the available options; change `WPE_HOST_PORT` if `19433` is already in use.

For HTTPS with a private CA, put the certificate in `./ssl/` on the Docker host, set `HA_CA_CERT_FILE=/ssl/<certificate-name>` in `.env`, and uncomment the `/ssl` volume in `docker-compose.yml`.

## Connect Home Assistant MCP

1. Enable dashboard screenshots in Home Assistant MCP. Follow the [Home Assistant MCP screenshot guide](https://github.com/homeassistant-ai/ha-mcp/blob/master/docs/beta.md) for its current feature setup.
2. Set the screenshot engine URL in MCP:
   - If MCP and the Compose service share a Docker network, use `http://wpe-screenshot-engine:8080`.
   - Otherwise, use `http://<engine-host>:<published-port>`. The default port is `19433`. For the Home Assistant App, find the published port under **WPE Screenshot Engine → Info → Network**.

   The Compose file creates its own Docker network. A separate MCP Compose project does not join it automatically; use the host URL unless you attach both containers to a shared network. Example: `http://192.168.1.2:19433`. Replace `192.168.1.2` with the engine host address reachable by MCP. Do not use `localhost` unless MCP and the engine share a network namespace.

3. For the Home Assistant MCP app, enter the URL in **Server Settings → Advanced → Operations**. The setting applies immediately. Supervisor auto-discovery currently looks for the Puppet app, so it will not discover WPE Screenshot Engine automatically.

For a Docker-based MCP installation, set `HAMCP_DASHBOARD_SCREENSHOT_ENGINE_URL` to the URL above in the MCP container environment and restart that container. The screenshot client chooses the dashboard path on each request; the engine's readiness route and text do not restrict requested routes.

## Verify the setup

Check that the engine is running by opening `http://<engine-host>:<published-port>/healthz` or running:

```sh
curl --fail http://192.168.1.2:19433/healthz
```

Replace the example host and port with values reachable from your machine. A successful health check confirms the HTTP service is running. To check rendering, request a dashboard route, for example:

```text
http://192.168.1.2:19433/lovelace/energy?viewport=1280x800
```

Replace the host and route with your own. A successful request returns an image. The Home Assistant token's user must be able to view the requested dashboard.

## Screenshots

These screenshots were captured by WPE Screenshot Engine from a disposable Home Assistant instance configured with fictional demo entities and built-in Sections, Tile, and Gauge cards. No screenshots or data from a personal Home Assistant instance are used. The matching Lovelace dashboard and demo entity definitions are in [`tests/fixtures`](tests/fixtures).

<p align="center">
  <img src="screenshots/mock-lovelace-desktop-light.png" alt="Demo Home dashboard captured from Home Assistant in desktop light mode" width="720">
</p>

Desktop: [dark](screenshots/mock-lovelace-desktop-dark.png) · [125% zoom](screenshots/mock-lovelace-desktop-zoom-125.png)

Tablet: [landscape light](screenshots/mock-lovelace-tablet-light.png) · [landscape dark](screenshots/mock-lovelace-tablet-dark.png) · [portrait light](screenshots/mock-lovelace-tablet-portrait-light.png)

Mobile: [portrait light](screenshots/mock-lovelace-mobile-light.png) · [portrait dark](screenshots/mock-lovelace-mobile-dark.png) · [landscape](screenshots/mock-lovelace-mobile-landscape-light.png)

Internal page: [Home Assistant Settings](screenshots/mock-home-assistant-settings-light.png)

Dashboard source: [Lovelace YAML](tests/fixtures/mock_lovelace_dashboard.yaml) · [demo entity definitions](tests/fixtures/mock_lovelace_entities.yaml)

## HTTP API

The API accepts `GET` requests. The request path selects a safe relative Home Assistant route to capture. For example:

```text
GET http://<engine-host>:<published-port>/lovelace/energy?viewport=1280x800&zoom=1.0&wait=2500&format=png
```

| Parameter | Supported value | Default |
| --- | --- | --- |
| `viewport` | Width and height from 64 to 4096 px, or `WIDTHxauto`. Auto-height captures the full document up to 4000 px. | `1280x800` |
| `zoom` | `0.1` to `5.0` | `1.0` |
| `wait` | `0` to `30000` milliseconds | `2500` |
| `format` | `png`, `jpeg`, `webp`, or `bmp` | `png` |
| `theme` | Home Assistant theme name. | None |
| `dark` | Bare `dark`, `true`, `1`, or `yes` requests dark mode; `false`, `0`, or `no` requests light mode. When omitted, Home Assistant's current setting is preserved. | Unchanged |
| `lang` | Home Assistant language code. | None |

The endpoint returns the requested image MIME type. `viewport=1280xauto` is the full-page form used by Home Assistant MCP. Very tall pages are clipped at 4000 px, with `X-WPE-Screenshot-Truncated: true` on the response. Full-page capture returns 422 for vertically scrolling inner containers. Fixed and sticky elements are laid out as ordinary page content during a full-page capture. The raw capture buffer is limited to 60 MiB, so the largest width and height values cannot always be combined. Encoded images are limited to 20 MiB. Theme, dark-mode, and language overrides apply to the returned image without changing saved Home Assistant user preferences. A full client disconnect cancels its capture; a request-side TCP half-close can still receive the response.

`GET /healthz` checks whether the service is running without starting the renderer. `GET /readyz` checks renderer readiness and may start the renderer if it was released while idle.

## Security

The screenshot API has no inbound authentication or route allowlist. Anyone who can reach its port can request screenshots of any safe route available to the token's Home Assistant user, including account or configuration pages. Keep the published port on a trusted network and use a dedicated, low-privilege Home Assistant user for the token.

The renderer validates screenshot paths and blocks cross-origin main-frame and frame navigations before they begin, then resets the view after a blocked redirect. A transient `about:blank` navigation is ignored. Ordinary external subresource requests made by a dashboard are not filtered. HTTPS certificates must be trusted by the system CA store or by the optional private CA configured above.

Home Assistant apps restrict nested Linux namespaces, which prevents WebKit's Bubblewrap process sandbox from starting. The app disables that inner sandbox so rendering works; Supervisor's outer container restrictions remain enabled. The renderer runs as UID `10001` with no Linux capabilities. This reduces browser-process isolation, so use trusted dashboards. The app removes its temporary token file after reading it and before starting WebKit. WebKit's [upstream sandbox override](https://bugs.webkit.org/show_bug.cgi?id=250232) is deliberately named to flag this security tradeoff.

## Troubleshooting

- **Engine reports no token:** for the Home Assistant App, set the access token in its **Configuration** tab. For Compose, check that `secrets/ha_token` exists and contains the token.
- **Login page or authorization error:** check `HA_URL` or the app's Home Assistant URL, then confirm the token's user can open the requested dashboard.
- **Dashboard never becomes ready:** confirm the dashboard route is valid. If readiness text is set, it is checked only on the configured readiness route.
- **MCP cannot connect:** check the configured engine URL and confirm MCP can reach that host and port. Use service DNS only on a shared Docker network; otherwise use the published host port.
- **App port conflict:** change the app's published port under **Info → Network**, or change `WPE_HOST_PORT` in Compose, and update the MCP URL to match.

## Development

Build the app image and run the HTTP and security regression tests on Ubuntu. Install Python 3, Docker with its daemon running, `pre-commit`, `cppcheck`, `shellcheck`, and `yamllint` first. The Home Assistant app configuration hook also uses Docker.

```sh
docker build --tag wpe-screenshot-engine:dev ./wpe_screenshot_engine
python3 -m unittest discover -s tests -v
pre-commit install
pre-commit run --all-files
```

GitHub Actions builds and publishes installable images for `amd64` and `arm64` when a GitHub release is published. See the [app changelog](wpe_screenshot_engine/CHANGELOG.md) for release history. [.env.example](.env.example) documents environment variables for Compose and direct Docker deployments; the Home Assistant App reads its settings from its Configuration tab.
