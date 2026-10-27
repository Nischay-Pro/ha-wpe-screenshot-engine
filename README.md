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
- **Full-page height limit:** maximum page height for one full-page image; default `12000` px, configurable from `1000` to `12000` px.
- **Full-page tile wait:** delay after each page scroll so lazy-loaded cards can paint; default `250` ms, configurable from `0` to `5000` ms.
- **Frontend option timeout:** maximum time Home Assistant has to apply a requested language, theme, or dark-mode override; default `10000` ms, configurable from `1000` to `60000` ms.
- **Allowed hosts:** optional comma-separated source IPs or CIDR ranges. Leave blank to allow all clients. Use the MCP client's source address as seen by the engine.

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
3. Create `secrets/ha_token` and paste the token created above into it. Compose mounts this file as a Docker secret; the token is not put in the container environment. A symlink is supported if it resolves to a readable regular file. `.env` and `secrets/` are gitignored.
4. Start the sidecar:

   ```sh
   docker compose -f docker-compose.yml up -d
   ```

The Home Assistant App and Compose have the same screenshot options and defaults. The app stores the token in its masked field and sets the host port in **Info → Network**; Compose uses `secrets/ha_token` and `WPE_HOST_PORT`. The sidecar listens on port `8080` inside Docker and publishes host port `19433` by default. `.env.example` describes the available options; change `WPE_HOST_PORT` if `19433` is already in use. Set `ALLOWED_HOSTS` in `.env` to restrict client source IPs, for example `192.168.1.25,192.168.1.0/24`.

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

### Screenshot requests

The API accepts `GET` requests. The request path selects a safe relative Home Assistant route to capture. For example:

```text
GET http://<engine-host>:<published-port>/lovelace/energy?viewport=1280x800&zoom=1.0&wait=2500&format=png
```

| Parameter | Supported value | Default |
| --- | --- | --- |
| `viewport` | Width and height from 64 to 4096 px, or `WIDTHxauto`. Auto-height captures the page at the requested width up to the configured full-page height limit. | `1280x800` |
| `zoom` | `0.1` to `5.0` | `1.0` |
| `wait` | `0` to `30000` milliseconds | `2500` |
| `format` | `png`, `jpeg`, `webp`, or `bmp` | `png` |
| `theme` | Home Assistant theme name. | None |
| `dark` | Bare `dark`, `true`, `1`, or `yes` requests dark mode; `false`, `0`, or `no` requests light mode. When omitted, Home Assistant's current setting is preserved. | Unchanged |
| `lang` | Home Assistant language code. | None |

Each successful request returns one image in the requested format. Theme, dark-mode, and language overrides affect only that image; they do not change saved Home Assistant preferences. An unavailable theme or an override Home Assistant does not apply within the configured timeout returns HTTP 422. If Home Assistant's theme data has not loaded before that timeout, the engine returns HTTP 504 because the frontend may still be loading. A full client disconnect cancels its capture. A request-side TCP half-close can still receive the response.

### Full-page captures

Use `viewport=WIDTHxauto` to capture a full page at the requested width. Home Assistant MCP uses this form. The engine measures page height at that width, then stitches viewport tiles into one image.

- **Memory:** PNG tiles are streamed into the encoder, avoiding a second full-height raw pixel buffer. Other formats use a full-page raw buffer.
- **Height and size:** requests over the configured height or pixel budget return HTTP 413; the engine does not return a clipped image. The effective height limit depends on width. The 20 MiB encoded-image limit and render deadline also apply, so detailed or slow pages may fail explicitly. `/capabilities?width=1280` reports the effective height limit for that width.
- **Lazy-loaded cards:** `full_page_tile_wait_ms` in the Home Assistant App or `FULL_PAGE_TILE_WAIT_MS` in Compose sets the delay after each scroll, so cards have time to paint before capture.
- **Frontend options:** `frontend_options_timeout_ms` in the Home Assistant App or `FRONTEND_OPTIONS_TIMEOUT_MS` in Compose sets how long language, theme, and dark-mode overrides can take to apply.
- **Zoom rounding:** small gaps caused by fractional zoom are filled by repeating the last captured row. A larger gap means the page did not scroll far enough and returns HTTP 422.
- **Layout limits:** vertically scrolling inner containers return HTTP 422. Fixed and sticky elements are laid out as ordinary page content in the full-page image.

### Health and capabilities

- **`GET /healthz`:** returns 200 while the HTTP service is running.
- **`GET /readyz`:** returns 200 when the renderer is ready or intentionally released while idle. If renderer recreation previously failed, the probe retries and returns 503 only while recovery continues to fail. Probes do not recreate an intentionally idle renderer; a screenshot request does.
- **`GET /capabilities?width=1280`:** reports the engine version, renderer state, supported formats, configured settings, and effective height limit for that width. It does not expose the Home Assistant URL, dashboard path, or token.

## Security

### Network access

The screenshot API has no authentication. By default, it listens on all IPv4 and IPv6 interfaces and accepts requests from any reachable client. A client can request screenshots of any safe dashboard route available to the Home Assistant user whose token the engine uses, including account or configuration pages.

To limit which clients can connect, configure **Allowed hosts** in the Home Assistant App or `ALLOWED_HOSTS` in Compose. Enter comma-separated IPv4/IPv6 addresses or CIDR ranges; leave the setting empty to allow all clients. The filter checks the TCP peer address and ignores `X-Forwarded-For` and similar headers. If MCP connects through a proxy, allow the proxy's source address. `/healthz` and `/readyz` remain reachable for container health checks. Keep the published port on a trusted network and use a dedicated, low-privilege Home Assistant user for the token.

### Dashboard requests

The engine accepts safe relative dashboard paths and applies its navigation policy to every document frame. It permits the configured Home Assistant origin and transient `about:blank`. It blocks other document navigations, including cross-origin frames and `about:srcdoc`, `blob:`, or `data:` frames. A dashboard that requires one of these frames will fail capture with HTTP 502. Ordinary external subresource requests, such as images or scripts loaded by a dashboard, are not filtered. HTTPS certificates must be trusted by the system CA store or by the optional private CA configured above.

### Process isolation

Home Assistant Apps restrict nested Linux namespaces, which prevents WebKit's Bubblewrap process sandbox from starting. The engine disables that inner sandbox so rendering works; Supervisor's outer container restrictions remain enabled. The renderer runs as UID `10001` with no Linux capabilities. This reduces browser-process isolation, so use trusted dashboards. The app removes its temporary token file after reading it and removes the token bootstrap script from WebKit after the first successful readiness check. If Home Assistant later redirects a capture to sign-in, the engine recreates the view, restores the bootstrap script, and retries once. The active Home Assistant web session and engine process retain the token for authentication and renderer recovery. WebKit does not guarantee that released script memory is securely erased. WebKit's [upstream sandbox override](https://bugs.webkit.org/show_bug.cgi?id=250232) is deliberately named to flag this tradeoff.

## Troubleshooting

- **Engine reports no token:** for the Home Assistant App, set the access token in its **Configuration** tab. For Compose, check that `secrets/ha_token` exists and contains the token.
- **Compose token file is rejected:** the token path must resolve to a readable regular file. Symlink targets that are missing, unreadable, or not regular files are rejected.
- **Login page or authorization error:** the engine retries one sign-in redirect automatically. If capture still fails, check `HA_URL` or the app's Home Assistant URL and confirm the token's user can open the requested dashboard.
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
