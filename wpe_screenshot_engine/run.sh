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
HA_CA_CERT_FILE="${HA_CA_CERT_FILE:-}"
OPTIONS_FILE=/data/options.json

# Supervisor normally mounts options before starting an app. Give a fresh
# installation a short grace period for that mount before checking the legacy
# token-file path, which otherwise produces a misleading first-start failure.
if [ -d /data ] && [ ! -e "$OPTIONS_FILE" ]; then
    printf '%s\n' "startup: waiting up to 10 seconds for Supervisor app options"
    attempts=0
    while [ ! -e "$OPTIONS_FILE" ] && [ "$attempts" -lt 50 ]; do
        sleep 0.2
        attempts=$((attempts + 1))
    done
fi

if [ -e "$OPTIONS_FILE" ]; then
    if [ ! -r "$OPTIONS_FILE" ]; then
        echo "startup error: Home Assistant app settings are not readable" >&2
        exit 78
    fi
    printf '%s\n' "startup: Supervisor app options loaded"

    read_option() {
        jq -er --arg key "$1" '.[$key] // empty' "$OPTIONS_FILE" 2>/dev/null
    }
    read_optional_option() {
        jq -r --arg key "$1" '.[$key] // ""' "$OPTIONS_FILE" 2>/dev/null
    }

    HA_URL="${HA_URL:-$(read_option home_assistant_url)}"
    HA_CA_CERT_FILE="${HA_CA_CERT_FILE:-$(read_optional_option home_assistant_ca_certificate)}"
    if ! OPTIONS_TOKEN="$(read_option home_assistant_token)"; then
        echo "startup error: set the Home Assistant access token in the app Configuration tab before starting" >&2
        exit 78
    fi
    DASHBOARD_PATH="${DASHBOARD_PATH:-$(read_option dashboard_path)}"
    EXPECTED_TEXT="${EXPECTED_TEXT-$(read_optional_option expected_text)}"
    RENDER_DEADLINE_SECONDS="${RENDER_DEADLINE_SECONDS:-$(read_option render_deadline_seconds)}"
    DASHBOARD_REUSE_IDLE_SECONDS="${DASHBOARD_REUSE_IDLE_SECONDS:-$(read_option dashboard_reuse_idle_seconds)}"
    WEBKIT_PROCESS_MEMORY_LIMIT_MB="${WEBKIT_PROCESS_MEMORY_LIMIT_MB:-$(read_option webkit_process_memory_limit_mb)}"
    MAX_AUTO_HEIGHT="${MAX_AUTO_HEIGHT:-$(read_optional_option max_auto_height)}"
    FULL_PAGE_TILE_WAIT_MS="${FULL_PAGE_TILE_WAIT_MS:-$(read_optional_option full_page_tile_wait_ms)}"
    FRONTEND_OPTIONS_TIMEOUT_MS="${FRONTEND_OPTIONS_TIMEOUT_MS:-$(read_optional_option frontend_options_timeout_ms)}"
    ALLOWED_HOSTS="${ALLOWED_HOSTS:-$(read_optional_option allowed_hosts)}"
else
    TOKEN_SOURCE="${HA_TOKEN_FILE:-/run/secrets/ha_token}"
fi

if [ -n "$HA_CA_CERT_FILE" ]; then
    case "$HA_CA_CERT_FILE" in
        /ssl/*) ;;
        *)
            echo "startup error: Home Assistant CA certificate must be stored under /ssl" >&2
            exit 78
            ;;
    esac
    if ! HA_CA_CERT_FILE="$(realpath -e -- "$HA_CA_CERT_FILE")"; then
        echo "startup error: Home Assistant CA certificate path does not exist" >&2
        exit 78
    fi
    case "$HA_CA_CERT_FILE" in
        /ssl/*) ;;
        *)
            echo "startup error: Home Assistant CA certificate must resolve inside /ssl" >&2
            exit 78
            ;;
    esac
    if [ ! -f "$HA_CA_CERT_FILE" ] || [ -L "$HA_CA_CERT_FILE" ] ||
       [ ! -r "$HA_CA_CERT_FILE" ]; then
        echo "startup error: Home Assistant CA certificate must be a readable regular file" >&2
        exit 78
    fi
    cert_bytes=$(wc -c < "$HA_CA_CERT_FILE")
    if [ "$cert_bytes" -le 0 ] || [ "$cert_bytes" -gt 65536 ]; then
        echo "startup error: Home Assistant CA certificate must be between 1 and 65536 bytes" >&2
        exit 78
    fi
    if ! openssl x509 -in "$HA_CA_CERT_FILE" -noout >/dev/null 2>&1; then
        echo "startup error: Home Assistant CA certificate is not a valid PEM certificate" >&2
        exit 78
    fi
    install -m 0644 "$HA_CA_CERT_FILE" \
        /usr/local/share/ca-certificates/home-assistant-private-ca.crt
    if ! update-ca-certificates >/dev/null; then
        echo "startup error: unable to add the Home Assistant CA certificate to the trust store" >&2
        exit 78
    fi
    printf '%s\n' "startup: custom Home Assistant CA certificate installed"
fi
unset HA_CA_CERT_FILE

export HA_URL="${HA_URL:-http://homeassistant:8123}"
export DASHBOARD_PATH="${DASHBOARD_PATH:-lovelace/energy}"
export EXPECTED_TEXT="${EXPECTED_TEXT-}"
export RENDER_DEADLINE_SECONDS="${RENDER_DEADLINE_SECONDS:-55}"
export DASHBOARD_REUSE_IDLE_SECONDS="${DASHBOARD_REUSE_IDLE_SECONDS:-300}"
export WEBKIT_PROCESS_MEMORY_LIMIT_MB="${WEBKIT_PROCESS_MEMORY_LIMIT_MB:-512}"
export MAX_AUTO_HEIGHT="${MAX_AUTO_HEIGHT:-12000}"
export FULL_PAGE_TILE_WAIT_MS="${FULL_PAGE_TILE_WAIT_MS:-250}"
export FRONTEND_OPTIONS_TIMEOUT_MS="${FRONTEND_OPTIONS_TIMEOUT_MS:-10000}"
export ALLOWED_HOSTS="${ALLOWED_HOSTS:-}"
export WPE_PLATFORM=headless

if [ "${WPE_FIXTURE_MODE:-0}" != "1" ]; then
    if [ -n "$OPTIONS_TOKEN" ]; then
        umask 077
        HA_TOKEN_FILE="$(mktemp /tmp/wpe-ha-token.XXXXXX)"
        printf '%s' "$OPTIONS_TOKEN" > "$HA_TOKEN_FILE"
        TOKEN_TEMP=1
    else
        if [ ! -r "$TOKEN_SOURCE" ]; then
            echo "startup error: no Home Assistant access token is available; configure the app token or mount a readable HA_TOKEN_FILE" >&2
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
printf '%s\n' "startup: starting WPE renderer as UID ${APP_UID}"

# Expand HOME and XDG_CACHE_HOME after dropping privileges.
# shellcheck disable=SC2016
exec /usr/bin/setpriv \
    --reuid="$APP_UID" \
    --regid="$APP_GID" \
    --clear-groups \
    --no-new-privs \
    -- \
    /bin/sh -eu -c 'mkdir -p "$HOME" "$XDG_CACHE_HOME"; exec /usr/local/bin/wpe-screenshot-engine'
