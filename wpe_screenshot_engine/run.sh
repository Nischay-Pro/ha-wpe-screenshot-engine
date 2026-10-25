#!/bin/sh
set -eu

if [ -r /data/options.json ]; then
    read_option() {
        jq -er --arg key "$1" '.[$key] // empty' /data/options.json
    }

    HA_URL="${HA_URL:-$(read_option home_assistant_url)}"
    if ! HA_TOKEN="$(read_option home_assistant_token)"; then
        echo "startup error: enter the Home Assistant access token in the app Configuration tab" >&2
        exit 78
    fi
    DASHBOARD_PATH="${DASHBOARD_PATH:-$(read_option dashboard_path)}"
    EXPECTED_TEXT="${EXPECTED_TEXT:-$(read_option expected_text)}"
    RENDER_DEADLINE_SECONDS="${RENDER_DEADLINE_SECONDS:-$(read_option render_deadline_seconds)}"
    DASHBOARD_REUSE_IDLE_SECONDS="${DASHBOARD_REUSE_IDLE_SECONDS:-$(read_option dashboard_reuse_idle_seconds)}"
    WEBKIT_PROCESS_MEMORY_LIMIT_MB="${WEBKIT_PROCESS_MEMORY_LIMIT_MB:-$(read_option webkit_process_memory_limit_mb)}"

    umask 077
    HA_TOKEN_FILE="$(mktemp /tmp/wpe-ha-token.XXXXXX)"
    printf '%s' "$HA_TOKEN" > "$HA_TOKEN_FILE"
    unset HA_TOKEN
fi

export HA_URL="${HA_URL:-http://homeassistant:8123}"
export HA_TOKEN_FILE="${HA_TOKEN_FILE:-/run/secrets/ha_token}"
export DASHBOARD_PATH="${DASHBOARD_PATH:-lovelace/energy}"
export EXPECTED_TEXT="${EXPECTED_TEXT:-Energy at a glance}"
export RENDER_DEADLINE_SECONDS="${RENDER_DEADLINE_SECONDS:-55}"
export DASHBOARD_REUSE_IDLE_SECONDS="${DASHBOARD_REUSE_IDLE_SECONDS:-300}"
export WEBKIT_PROCESS_MEMORY_LIMIT_MB="${WEBKIT_PROCESS_MEMORY_LIMIT_MB:-512}"
export WPE_PLATFORM=headless

if [ "${WPE_FIXTURE_MODE:-0}" != "1" ] && [ ! -r "$HA_TOKEN_FILE" ]; then
    echo "startup error: HA token file is not readable by the app user" >&2
    exit 78
fi

mkdir -p "$HOME" "$XDG_CACHE_HOME"
exec /usr/local/bin/wpe-screenshot-engine
