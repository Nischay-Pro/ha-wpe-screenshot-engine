#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "startup error: Home Assistant app settings must be read by the startup user" >&2
    exit 78
fi

APP_UID=10001
APP_GID=10001
OPTIONS_TOKEN=
TOKEN_SOURCE=
TOKEN_TEMP=0

if [ -e /data/options.json ]; then
    if [ ! -r /data/options.json ]; then
        echo "startup error: Home Assistant app settings are not readable" >&2
        exit 78
    fi

    read_option() {
        jq -er --arg key "$1" '.[$key] // empty' /data/options.json
    }

    HA_URL="${HA_URL:-$(read_option home_assistant_url)}"
    if ! OPTIONS_TOKEN="$(read_option home_assistant_token)"; then
        echo "startup error: enter the Home Assistant access token in the app Configuration tab" >&2
        exit 78
    fi
    DASHBOARD_PATH="${DASHBOARD_PATH:-$(read_option dashboard_path)}"
    EXPECTED_TEXT="${EXPECTED_TEXT:-$(read_option expected_text)}"
    RENDER_DEADLINE_SECONDS="${RENDER_DEADLINE_SECONDS:-$(read_option render_deadline_seconds)}"
    DASHBOARD_REUSE_IDLE_SECONDS="${DASHBOARD_REUSE_IDLE_SECONDS:-$(read_option dashboard_reuse_idle_seconds)}"
    WEBKIT_PROCESS_MEMORY_LIMIT_MB="${WEBKIT_PROCESS_MEMORY_LIMIT_MB:-$(read_option webkit_process_memory_limit_mb)}"
else
    TOKEN_SOURCE="${HA_TOKEN_FILE:-/run/secrets/ha_token}"
fi

export HA_URL="${HA_URL:-http://homeassistant:8123}"
export DASHBOARD_PATH="${DASHBOARD_PATH:-lovelace/energy}"
export EXPECTED_TEXT="${EXPECTED_TEXT:-Energy at a glance}"
export RENDER_DEADLINE_SECONDS="${RENDER_DEADLINE_SECONDS:-55}"
export DASHBOARD_REUSE_IDLE_SECONDS="${DASHBOARD_REUSE_IDLE_SECONDS:-300}"
export WEBKIT_PROCESS_MEMORY_LIMIT_MB="${WEBKIT_PROCESS_MEMORY_LIMIT_MB:-512}"
export WPE_PLATFORM=headless

if [ "${WPE_FIXTURE_MODE:-0}" != "1" ]; then
    if [ -n "$OPTIONS_TOKEN" ]; then
        umask 077
        HA_TOKEN_FILE="$(mktemp /tmp/wpe-ha-token.XXXXXX)"
        printf '%s' "$OPTIONS_TOKEN" > "$HA_TOKEN_FILE"
        TOKEN_TEMP=1
    else
        if [ ! -r "$TOKEN_SOURCE" ]; then
            echo "startup error: HA token file is not readable by the app user" >&2
            exit 78
        fi
        if [ ! -f "$TOKEN_SOURCE" ] || [ -L "$TOKEN_SOURCE" ]; then
            HA_TOKEN_FILE="$TOKEN_SOURCE"
        else
            umask 077
            HA_TOKEN_FILE="$(mktemp /tmp/wpe-ha-token.XXXXXX)"
            # Bound the copy; the renderer reports an oversized token safely.
            head -c 8193 "$TOKEN_SOURCE" > "$HA_TOKEN_FILE"
            TOKEN_TEMP=1
        fi
    fi
    if [ "$TOKEN_TEMP" -eq 1 ]; then
        chmod 0400 "$HA_TOKEN_FILE"
        chown "$APP_UID:$APP_GID" "$HA_TOKEN_FILE"
    fi
    unset OPTIONS_TOKEN TOKEN_SOURCE
else
    HA_TOKEN_FILE="${HA_TOKEN_FILE:-/run/secrets/ha_token}"
    unset OPTIONS_TOKEN TOKEN_SOURCE
fi
export HA_TOKEN_FILE

# Expand HOME and XDG_CACHE_HOME after dropping privileges.
# shellcheck disable=SC2016
exec /usr/bin/setpriv \
    --reuid="$APP_UID" \
    --regid="$APP_GID" \
    --clear-groups \
    --no-new-privs \
    -- \
    /bin/sh -eu -c 'mkdir -p "$HOME" "$XDG_CACHE_HOME"; exec /usr/local/bin/wpe-screenshot-engine'
