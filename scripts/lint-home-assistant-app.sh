#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cache_root="${PRE_COMMIT_HOME:-${XDG_CACHE_HOME:-$HOME/.cache/pre-commit}}/home-assistant-app-linter"
LINTER_VERSION=v2.21.1

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the Home Assistant app linter locally." >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "The Docker daemon must be running to run the Home Assistant app linter." >&2
  exit 1
fi

if [[ -d "${cache_root}/.git" ]]; then
  git -C "$cache_root" fetch --quiet --depth=1 origin \
    "refs/tags/${LINTER_VERSION}:refs/tags/${LINTER_VERSION}"
  git -C "$cache_root" checkout --quiet --force --detach "$LINTER_VERSION"
else
  mkdir -p "$(dirname "$cache_root")"
  git clone --quiet --depth=1 --single-branch --branch "$LINTER_VERSION" \
    https://github.com/frenck/action-app-linter.git "$cache_root"
fi

linter_revision="$(git -C "$cache_root" rev-parse HEAD)"
linter_image="local/home-assistant-app-linter:${linter_revision}"

if ! docker image inspect "$linter_image" >/dev/null 2>&1; then
  docker build --pull --quiet \
    --tag "$linter_image" \
    --file "$cache_root/src/Dockerfile" \
    "$cache_root/src"
fi

docker run --rm --network none \
  --env INPUT_PATH=/github/workspace/wpe_screenshot_engine \
  --env INPUT_COMMUNITY=false \
  --volume "$repo_root:/github/workspace:ro" \
  --workdir /github/workspace \
  "$linter_image"
