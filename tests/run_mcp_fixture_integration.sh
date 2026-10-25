#!/bin/sh
set -eu

MCP_COMMIT=23f3508a5aa0d0a911140518081ca7be7bc65a3b
CAPTURE_SHA256=f298cab98dd7615638d699e41a313cc0444c4c7777a4f6b6674d744afe2f206e
BUILD_IMAGE=wpe-screenshot-engine-build:0.1.3
ENGINE_IMAGE="${WPE_IMAGE:-wpe-screenshot-engine:0.1.3}"
NETWORK_NAME="wpe-mcp-fixture-$$"
ENGINE_NAME="wpe-mcp-fixture-$$"
CAPTURE_SOURCE="/tmp/ha-mcp-capture-pinned-$$.py"

cleanup() {
    docker rm --force "$ENGINE_NAME" >/dev/null 2>&1 || true
    docker network rm "$NETWORK_NAME" >/dev/null 2>&1 || true
    rm -f "$CAPTURE_SOURCE"
}
trap cleanup EXIT HUP INT TERM

curl -fsSL \
    "https://raw.githubusercontent.com/homeassistant-ai/ha-mcp/$MCP_COMMIT/src/ha_mcp/dashboard_screenshot/capture.py" \
    -o "$CAPTURE_SOURCE"
printf '%s  %s\n' "$CAPTURE_SHA256" "$CAPTURE_SOURCE" | sha256sum -c -

if ! docker image inspect "$ENGINE_IMAGE" >/dev/null 2>&1; then
    if [ -n "${WPE_IMAGE:-}" ]; then
        docker pull "$ENGINE_IMAGE"
    else
        docker build -t "$ENGINE_IMAGE" ./wpe_screenshot_engine
    fi
fi
docker build --target build -t "$BUILD_IMAGE" ./wpe_screenshot_engine
docker network create "$NETWORK_NAME" >/dev/null

docker run --detach --init --name "$ENGINE_NAME" \
    --network "$NETWORK_NAME" --network-alias wpe-fixture \
    --read-only --tmpfs /tmp:rw,nosuid,size=512m \
    --cap-drop ALL \
    --security-opt seccomp=unconfined \
    --security-opt no-new-privileges:true \
    --pids-limit 128 --memory 768m --cpus 2 \
    --env WPE_FIXTURE_MODE=1 \
    --env HA_URL=http://fixture.local \
    --env DASHBOARD_PATH=lovelace/energy \
    --env 'EXPECTED_TEXT=Plug Power' \
    --env RENDER_DEADLINE_SECONDS=10 \
    "$ENGINE_IMAGE" >/dev/null

docker run --rm --network "$NETWORK_NAME" \
    --mount "type=bind,src=$PWD/tests,dst=/tests,readonly" \
    --mount "type=bind,src=$CAPTURE_SOURCE,dst=/tmp/ha-mcp-capture-pinned.py,readonly" \
    --env DEBIAN_FRONTEND=noninteractive \
    --env HA_MCP_CAPTURE_SOURCE=/tmp/ha-mcp-capture-pinned.py \
    --env MCP_ENGINE_URL=http://wpe-fixture:8080 \
    --entrypoint /bin/sh \
    "$BUILD_IMAGE" -lc \
    'apt-get update >/dev/null && apt-get install -y --no-install-recommends python3-httpx >/dev/null && python3 /tests/run_pinned_mcp_capture.py'
