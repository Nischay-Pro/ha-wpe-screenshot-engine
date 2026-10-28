#!/bin/sh
set -eu

# Run inside the Home Assistant apps devcontainer with Supervisor running.
# CI prepares tests/.upgrade-baseline from the latest published release before
# invoking this script. The checked-out app is installed fresh, then the prior
# release is installed and upgraded to a local build of the checked-out source.
#
# For a manual run, prepare the same baseline fixture first:
#   previous_version="$(gh api repos/Nischay-Pro/ha-wpe-screenshot-engine/releases/latest --jq '.tag_name')"
#   git fetch --depth=1 origin "refs/tags/${previous_version}:refs/tags/${previous_version}"
#   mkdir -p tests/.upgrade-baseline
#   git archive "${previous_version}" wpe_screenshot_engine | tar -x -C tests/.upgrade-baseline
#   printf '%s\n' "${previous_version}" > tests/.upgrade-baseline/VERSION
#
#   npx --yes @devcontainers/cli up --workspace-folder . --config tests/.devcontainer.json
#   npx --yes @devcontainers/cli exec --workspace-folder . --config tests/.devcontainer.json \
#     bash -lc 'nohup supervisor_run >/tmp/wpe-supervisor.log 2>&1 </dev/null &'
#   npx --yes @devcontainers/cli exec --workspace-folder . --config tests/.devcontainer.json \
#     sh tests/run_supervisor_install_upgrade_integration.sh
#   npx --yes @devcontainers/cli down --workspace-folder . --config tests/.devcontainer.json

REPO_ROOT=$(pwd)
SOURCE_DIR="$REPO_ROOT/wpe_screenshot_engine"
BASELINE_DIR="$REPO_ROOT/tests/.upgrade-baseline/wpe_screenshot_engine"
BASELINE_VERSION_FILE="$REPO_ROOT/tests/.upgrade-baseline/VERSION"
APP_DIR="$REPO_ROOT/wpe_screenshot_engine_upgrade_e2e"
APP_SLUG=wpe_screenshot_engine_upgrade_e2e
STORE_SLUG="local_$APP_SLUG"
APP_CONTAINER="app_$STORE_SLUG"
FRESH_APP_DIR="$REPO_ROOT/wpe_screenshot_engine_install_e2e"
FRESH_APP_SLUG=wpe_screenshot_engine_install_e2e
FRESH_STORE_SLUG="local_$FRESH_APP_SLUG"
FRESH_APP_CONTAINER="app_$FRESH_STORE_SLUG"
APP_INSTALLED=0
FRESH_APP_INSTALLED=0

if [ ! -f "$REPO_ROOT/repository.yaml" ] \
    || [ ! -f "$SOURCE_DIR/config.yaml" ] \
    || [ ! -f "$BASELINE_DIR/config.yaml" ] \
    || [ ! -f "$BASELINE_VERSION_FILE" ]; then
    echo "Run from the mounted app repository with the previous-release fixture prepared." >&2
    exit 2
fi
if [ -e "$APP_DIR" ] || [ -e "$FRESH_APP_DIR" ]; then
    echo "Refusing to overwrite an existing temporary Home Assistant app path." >&2
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

UPGRADE_FROM_VERSION=$(cat "$BASELINE_VERSION_FILE")
if ! printf '%s\n' "$UPGRADE_FROM_VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "Previous release version is not numeric semantic versioning: $UPGRADE_FROM_VERSION" >&2
    exit 2
fi
BASELINE_MAJOR=${UPGRADE_FROM_VERSION%%.*}
BASELINE_REMAINDER=${UPGRADE_FROM_VERSION#*.}
BASELINE_MINOR=${BASELINE_REMAINDER%%.*}
BASELINE_PATCH=${BASELINE_REMAINDER##*.}
CANDIDATE_VERSION="$BASELINE_MAJOR.$BASELINE_MINOR.$((BASELINE_PATCH + 1))"

cleanup() {
    if [ "$FRESH_APP_INSTALLED" -eq 1 ]; then
        ha --no-progress apps stop "$FRESH_STORE_SLUG" >/dev/null 2>&1 || true
        ha --no-progress apps uninstall "$FRESH_STORE_SLUG" >/dev/null 2>&1 || true
    fi
    if [ "$APP_INSTALLED" -eq 1 ]; then
        ha --no-progress apps stop "$STORE_SLUG" >/dev/null 2>&1 || true
        ha --no-progress apps uninstall "$STORE_SLUG" >/dev/null 2>&1 || true
    fi
    rm -rf -- "$APP_DIR" "$FRESH_APP_DIR"
    ha --no-progress store reload >/dev/null 2>&1 || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

install_previous_release_manifest() {
    cp -a "$BASELINE_DIR" "$APP_DIR"
    sed -i \
        -e 's/^slug: \("wpe_screenshot_engine"\|wpe_screenshot_engine\)$/slug: "wpe_screenshot_engine_upgrade_e2e"/' \
        -e 's/^name: \("WPE Screenshot Engine"\|WPE Screenshot Engine\)$/name: "WPE Screenshot Engine Upgrade E2E"/' \
        "$APP_DIR/config.yaml"

    if ! grep -Fq 'image: ghcr.io/nischay-pro/ha-wpe-screenshot-engine' \
        "$APP_DIR/config.yaml"; then
        echo "Previous release manifest does not reference the published WPE image." >&2
        exit 1
    fi
}

install_fresh_candidate_manifest() {
    cp -a "$SOURCE_DIR" "$FRESH_APP_DIR"
    sed -i \
        -e 's/^slug: \("wpe_screenshot_engine"\|wpe_screenshot_engine\)$/slug: "wpe_screenshot_engine_install_e2e"/' \
        -e 's/^name: \("WPE Screenshot Engine"\|WPE Screenshot Engine\)$/name: "WPE Screenshot Engine Install E2E"/' \
        -e '/^image:/d' \
        "$FRESH_APP_DIR/config.yaml"
}

install_candidate_manifest() {
    rm -rf -- "$APP_DIR"
    cp -a "$SOURCE_DIR" "$APP_DIR"
    sed -i \
        -e 's/^slug: \("wpe_screenshot_engine"\|wpe_screenshot_engine\)$/slug: "wpe_screenshot_engine_upgrade_e2e"/' \
        -e 's/^name: \("WPE Screenshot Engine"\|WPE Screenshot Engine\)$/name: "WPE Screenshot Engine Upgrade E2E"/' \
        -e "s/^version: .*/version: $CANDIDATE_VERSION/" \
        -e '/^image:/d' \
        "$APP_DIR/config.yaml"
}

set_non_default_options() {
    docker exec -i hassio_cli sh -eu -c '
        : "${SUPERVISOR_TOKEN:?Supervisor token is not available in hassio_cli}"
        curl --fail --silent --show-error \
            -X POST \
            -H "Authorization: Bearer ${SUPERVISOR_TOKEN}" \
            -H "Content-Type: application/json" \
            --data-binary @- \
            "http://supervisor/addons/$1/options" >/dev/null
    ' sh "$1" <<'JSON'
{
  "options": {
    "home_assistant_url": "http://homeassistant:8123",
    "home_assistant_token": "integration-test-placeholder-not-a-real-token",
    "home_assistant_ca_certificate": "",
    "dashboard_path": "lovelace/upgrade-check",
    "expected_text": "",
    "render_deadline_seconds": 42,
    "dashboard_reuse_idle_seconds": 123,
    "webkit_process_memory_limit_mb": 768,
    "max_auto_height": 8000,
    "full_page_tile_wait_ms": 375,
    "frontend_options_timeout_ms": 15000,
    "allowed_hosts": "127.0.0.1"
  }
}
JSON
}

assert_saved_options() {
    app_info=$(ha --no-progress apps info "$1" --raw-json)
    if ! printf '%s\n' "$app_info" | jq -e '
            .data.options as $options
            | $options.home_assistant_url == "http://homeassistant:8123"
            and $options.dashboard_path == "lovelace/upgrade-check"
            and $options.render_deadline_seconds == 42
            and $options.dashboard_reuse_idle_seconds == 123
            and $options.webkit_process_memory_limit_mb == 768
            and $options.max_auto_height == 8000
            and $options.full_page_tile_wait_ms == 375
            and $options.frontend_options_timeout_ms == 15000
            and $options.allowed_hosts == "127.0.0.1"
        ' >/dev/null; then
        echo "Supervisor did not preserve the configured non-default options for $1." >&2
        printf '%s\n' "$app_info" \
            | jq '{state: .data.state, version: .data.version, options: (.data.options | del(.home_assistant_token))}' >&2
        return 1
    fi
    echo "PASS: Supervisor preserved non-default options for $1."
}

assert_saved_options_validate() {
    validation=$(docker exec hassio_cli sh -eu -c '
        : "${SUPERVISOR_TOKEN:?Supervisor token is not available in hassio_cli}"
        curl --fail --silent --show-error \
            -X POST \
            -H "Authorization: Bearer ${SUPERVISOR_TOKEN}" \
            -H "Content-Type: application/json" \
            --data-binary "{}" \
            "http://supervisor/addons/$1/options/validate"
    ' sh "$1")
    if ! printf '%s\n' "$validation" | jq -e '.data.valid == true' >/dev/null; then
        printf 'Supervisor rejected the app options after update: %s\n' "$validation" >&2
        return 1
    fi
    echo "PASS: Supervisor validates the saved options for $1."
}

wait_for_health() {
    attempt=0
    until docker exec "$1" wget -qO- http://127.0.0.1:8080/healthz \
        | grep -qx 'ok'; do
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 60 ]; then
            docker logs "$1" >&2 || true
            echo "App did not become healthy after install/update." >&2
            exit 1
        fi
        sleep 1
    done
}

assert_token_delivery() {
    if docker logs "$1" 2>&1 \
        | grep -Fq 'HA token file is not readable by the app user'; then
        docker logs "$1" >&2
        echo "Supervisor did not deliver a readable app token file." >&2
        exit 1
    fi
    if ! docker exec "$1" sh -c \
        'test -z "$(find /tmp -maxdepth 1 -type f -name "wpe-ha-token.*" -print -quit)"'; then
        echo "Temporary Home Assistant token file was not removed after startup." >&2
        exit 1
    fi
}

echo "Installing the current source as a fresh app through Supervisor..."
install_fresh_candidate_manifest
ha --no-progress store reload >/dev/null
ha --no-progress store list --raw-json \
    | jq -e --arg slug "$FRESH_STORE_SLUG" \
        '.data.addons[] | select(.slug == $slug and .build == true)' >/dev/null
FRESH_APP_INSTALLED=1
ha --no-progress apps install "$FRESH_STORE_SLUG" >/dev/null
set_non_default_options "$FRESH_STORE_SLUG"
ha --no-progress apps start "$FRESH_STORE_SLUG" >/dev/null
ha --no-progress apps info "$FRESH_STORE_SLUG" --raw-json \
    | jq -e '.data.state == "started"' >/dev/null
wait_for_health "$FRESH_APP_CONTAINER"
assert_saved_options "$FRESH_STORE_SLUG"
assert_token_delivery "$FRESH_APP_CONTAINER"
ha --no-progress apps stop "$FRESH_STORE_SLUG" >/dev/null
ha --no-progress apps uninstall "$FRESH_STORE_SLUG" >/dev/null
FRESH_APP_INSTALLED=0
rm -rf -- "$FRESH_APP_DIR"
ha --no-progress store reload >/dev/null
echo "PASS: Supervisor freshly built, installed, configured, and started the current app."

echo "Installing previous WPE release ${UPGRADE_FROM_VERSION} through Supervisor..."
install_previous_release_manifest
echo "Reloading the local app store with the previous-release manifest..."
ha --no-progress store reload
store_listing=$(ha --no-progress store list --raw-json)
if ! printf '%s\n' "$store_listing" \
    | jq -e --arg slug "$STORE_SLUG" \
        '.data.addons[] | select(.slug == $slug)' >/dev/null; then
    echo "Previous-release app $STORE_SLUG is missing from the Supervisor store." >&2
    printf '%s\n' "$store_listing" \
        | jq -r '.data.addons[]? | select(.slug | contains("wpe_screenshot_engine")) | [.slug, (.version // ""), (.build // false), (.image // "")] | @tsv' >&2
    exit 1
fi
APP_INSTALLED=1
ha --no-progress apps install "$STORE_SLUG" >/dev/null
set_non_default_options "$STORE_SLUG"
ha --no-progress apps start "$STORE_SLUG" >/dev/null
ha --no-progress apps info "$STORE_SLUG" --raw-json \
    | jq -e '.data.state == "started"' >/dev/null
wait_for_health "$APP_CONTAINER"
assert_saved_options "$STORE_SLUG"
assert_token_delivery "$APP_CONTAINER"

previous_image_version=$(docker inspect \
    --format '{{ index .Config.Labels "io.hass.version" }}' "$APP_CONTAINER")
if [ "$previous_image_version" != "$UPGRADE_FROM_VERSION" ]; then
    echo "Installed baseline image version $previous_image_version, expected $UPGRADE_FROM_VERSION." >&2
    exit 1
fi

echo "Updating the same app to PR source version ${CANDIDATE_VERSION}..."
install_candidate_manifest
echo "Reloading the local app store with the PR build manifest..."
ha --no-progress store reload
store_listing=$(ha --no-progress store list --raw-json)
if ! printf '%s\n' "$store_listing" \
    | jq -e --arg slug "$STORE_SLUG" \
        '.data.addons[] | select(.slug == $slug and .build == true)' >/dev/null; then
    echo "PR app $STORE_SLUG is not available as a locally built Supervisor app." >&2
    printf '%s\n' "$store_listing" \
        | jq -r '.data.addons[]? | select(.slug | contains("wpe_screenshot_engine")) | [.slug, (.version // ""), (.build // false), (.image // "")] | @tsv' >&2
    exit 1
fi
update_log=$(mktemp)
if ! ha --no-progress apps update "$STORE_SLUG" >"$update_log" 2>&1; then
    cat "$update_log" >&2
    rm -f -- "$update_log"
    echo "Supervisor failed to update the installed previous-release app." >&2
    exit 1
fi
cat "$update_log"
rm -f -- "$update_log"

app_info=$(ha --no-progress apps info "$STORE_SLUG" --raw-json)
if ! printf '%s\n' "$app_info" | jq -e '.data.state == "started"' >/dev/null; then
    printf '%s\n' "$app_info" \
        | jq '{state: .data.state, version: .data.version, update_available: .data.update_available}' >&2
    docker logs "$APP_CONTAINER" >&2 || true
    echo "Supervisor did not leave the updated app in the started state." >&2
    exit 1
fi
printf '%s\n' "$app_info" \
    | jq -r '"Supervisor app state after update: \(.data.state), version: \(.data.version // "unknown")"'
wait_for_health "$APP_CONTAINER"
echo "PASS: Updated app passed /healthz."
assert_saved_options "$STORE_SLUG"
assert_saved_options_validate "$STORE_SLUG"
assert_token_delivery "$APP_CONTAINER"
echo "PASS: Updated app removed the temporary token file."

candidate_image_version=$(docker inspect \
    --format '{{ index .Config.Labels "io.hass.version" }}' "$APP_CONTAINER")
if [ "$candidate_image_version" != "$CANDIDATE_VERSION" ]; then
    echo "Updated image version $candidate_image_version, expected $CANDIDATE_VERSION." >&2
    exit 1
fi

echo "PASS: Supervisor installed WPE ${UPGRADE_FROM_VERSION}, preserved non-default options, and updated to the PR build."
echo "PASS: The updated app passed options validation, /healthz, and token-file cleanup checks."
