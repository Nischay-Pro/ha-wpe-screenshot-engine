#!/bin/sh
set -eu

# Run inside the Home Assistant apps devcontainer with Supervisor running:
#   npx --yes @devcontainers/cli up --workspace-folder . --config tests/.devcontainer.json
#   npx --yes @devcontainers/cli exec --workspace-folder . --config tests/.devcontainer.json \
#     bash -lc 'nohup supervisor_run >/tmp/wpe-supervisor.log 2>&1 </dev/null &'
#   npx --yes @devcontainers/cli exec --workspace-folder . --config tests/.devcontainer.json \
#     sh tests/run_supervisor_install_integration.sh
#   npx --yes @devcontainers/cli down --workspace-folder . --config tests/.devcontainer.json
# The script copies the current app into the local store, removes its registry
# image reference so Supervisor builds this working tree, then installs it as a
# disposable Home Assistant app.

REPO_ROOT=$(pwd)
SOURCE_DIR="$REPO_ROOT/wpe_screenshot_engine"
APP_DIR="$REPO_ROOT/wpe_screenshot_engine_e2e"
APP_SLUG=wpe_screenshot_engine_e2e
STORE_SLUG="local_$APP_SLUG"
APP_CONTAINER="app_$STORE_SLUG"
APP_INSTALLED=0

if [ ! -f "$REPO_ROOT/repository.yaml" ] || [ ! -f "$SOURCE_DIR/config.yaml" ]; then
    echo "Run this script from the mounted Home Assistant app repository root." >&2
    exit 2
fi
if [ -e "$APP_DIR" ]; then
    echo "Refusing to overwrite existing path: $APP_DIR" >&2
    exit 2
fi
if ! command -v ha >/dev/null 2>&1 || ! command -v docker >/dev/null 2>&1; then
    echo "The Home Assistant apps devcontainer must provide ha and docker." >&2
    exit 2
fi
if ! docker inspect hassio_supervisor >/dev/null 2>&1; then
    echo "Home Assistant Supervisor is not running in this devcontainer." >&2
    exit 2
fi

cleanup() {
    if [ "$APP_INSTALLED" -eq 1 ]; then
        ha --no-progress apps stop "$STORE_SLUG" >/dev/null 2>&1 || true
        ha --no-progress apps uninstall "$STORE_SLUG" >/dev/null 2>&1 || true
    fi
    rm -rf -- "$APP_DIR"
    ha --no-progress store reload >/dev/null 2>&1 || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cp -a "$SOURCE_DIR" "$APP_DIR"
sed -i \
    -e 's/^slug: \("wpe_screenshot_engine"\|wpe_screenshot_engine\)$/slug: "wpe_screenshot_engine_e2e"/' \
    -e 's/^name: \("WPE Screenshot Engine"\|WPE Screenshot Engine\)$/name: "WPE Screenshot Engine E2E"/' \
    -e '/^image:/d' \
    "$APP_DIR/config.yaml"

ha --no-progress store reload >/dev/null
ha --no-progress store list --raw-json \
    | jq -e --arg slug "$STORE_SLUG" \
        '.data.addons[] | select(.slug == $slug and .build == true)' >/dev/null

echo "Installing current source through Home Assistant Supervisor..."
APP_INSTALLED=1
ha --no-progress apps install "$STORE_SLUG" >/dev/null

# This placeholder is intentionally invalid and must never be used outside the
# isolated integration-test instance. It verifies Supervisor option delivery
# and the app's secret-file startup path without a real HA credential.
docker exec -i hassio_cli sh -eu -c '
    : "${SUPERVISOR_TOKEN:?Supervisor token is not available in hassio_cli}"
    curl --fail --silent --show-error \
        -X POST \
        -H "Authorization: Bearer ${SUPERVISOR_TOKEN}" \
        -H "Content-Type: application/json" \
        --data-binary @- \
        "http://supervisor/addons/$1/options" >/dev/null
' sh "$STORE_SLUG" <<'JSON'
{
  "options": {
    "home_assistant_url": "http://homeassistant:8123",
    "home_assistant_token": "integration-test-placeholder-not-a-real-token",
    "home_assistant_ca_certificate": "",
    "dashboard_path": "lovelace/energy",
    "expected_text": "",
    "render_deadline_seconds": 55,
    "dashboard_reuse_idle_seconds": 300,
    "webkit_process_memory_limit_mb": 512,
    "max_auto_height": 12000,
    "full_page_tile_wait_ms": 250,
    "frontend_options_timeout_ms": 10000,
    "allowed_hosts": ""
  }
}
JSON

ha --no-progress apps start "$STORE_SLUG" >/dev/null
ha --no-progress apps info "$STORE_SLUG" --raw-json \
    | jq -e '.data.state == "started"' >/dev/null

echo "Waiting for the installed app's HTTP health endpoint..."
attempt=0
until docker exec "$APP_CONTAINER" wget -qO- http://127.0.0.1:8080/healthz \
    | grep -qx 'ok'; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 60 ]; then
        docker logs "$APP_CONTAINER" >&2 || true
        echo "Installed app did not become healthy." >&2
        exit 1
    fi
    sleep 1
done

if docker logs "$APP_CONTAINER" 2>&1 \
    | grep -Fq 'HA token file is not readable by the app user'; then
    docker logs "$APP_CONTAINER" >&2
    echo "Supervisor did not deliver a readable app token file." >&2
    exit 1
fi
if docker exec "$APP_CONTAINER" sh -c \
    'test -z "$(find /tmp -maxdepth 1 -type f -name "wpe-ha-token.*" -print -quit)"'; then
    :
else
    echo "Temporary Home Assistant token file was not removed after startup." >&2
    exit 1
fi

echo "PASS: Supervisor discovered, built, installed, configured, and started the current WPE app."
echo "PASS: /healthz responded and the temporary token file was removed."
