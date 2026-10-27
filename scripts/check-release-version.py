"""Keep the Home Assistant app version aligned with Release Please."""

import json
import re
import sys
from pathlib import Path


def main() -> int:
    manifest = json.loads(Path(".release-please-manifest.json").read_text())
    app_config = Path("wpe_screenshot_engine/config.yaml").read_text()
    match = re.search(r'^version:\s*["\']?([^"\'\s#]+)', app_config, re.MULTILINE)

    if match is None:
        print(
            "Could not find version in wpe_screenshot_engine/config.yaml",
            file=sys.stderr,
        )
        return 1

    app_version = match.group(1)
    release_version = manifest.get(".")
    if app_version != release_version:
        print(
            f"Home Assistant app version {app_version!r} does not match Release Please "
            f"manifest version {release_version!r}",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
