FROM debian:forky-slim AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      gcc \
      pkg-config \
      libwpewebkit-2.0-dev \
      libsoup-3.0-dev \
      libpng-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY src/main.c /build/main.c
COPY fixtures/dashboard.html /opt/wpe-engine/fixtures/dashboard.html
# pkg-config intentionally expands its output into separate compiler arguments.
# hadolint ignore=SC2046
RUN gcc -std=c11 -O2 -pipe -fPIE -pie \
    -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -Wall -Wextra -Wformat=2 -Werror \
    -Wl,-z,relro,-z,now \
    -o /build/wpe-screenshot-engine /build/main.c \
    $(pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-headless-2.0 libsoup-3.0 libpng16)

FROM debian:forky-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_ARCH=amd64
ARG BUILD_VERSION=0.1.7
LABEL io.hass.name="WPE Screenshot Engine" \
      io.hass.description="Private WPE WebKit engine for Home Assistant dashboard screenshots" \
      io.hass.type="app" \
      io.hass.version="${BUILD_VERSION}" \
      io.hass.arch="${BUILD_ARCH}" \
      io.hass.url="https://github.com/Nischay-Pro/ha-wpe-screenshot-engine" \
      org.opencontainers.image.source="https://github.com/Nischay-Pro/ha-wpe-screenshot-engine"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      ca-certificates \
      fonts-dejavu-core \
      jq \
      libgles2 \
      libpng16-16t64 \
      libsoup-3.0-0 \
      util-linux \
      wget \
      libwpewebkit-2.0-1 \
    && rm -rf /var/lib/apt/lists/* \
    && printf '%s\n' 'wpe:x:10001:10001:WPE renderer:/tmp/wpe-home:/usr/sbin/nologin' >> /etc/passwd \
    && printf '%s\n' 'wpe:x:10001:' >> /etc/group

# The Supervisor container's restrictions prevent WebKit's nested Bubblewrap
# namespace from starting. Keep the outer container restrictions enabled.
ENV WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 \
    HOME=/tmp/wpe-home \
    XDG_CACHE_HOME=/tmp/wpe-cache \
    TMPDIR=/tmp \
    WPE_PLATFORM=headless \
    MALLOC_ARENA_MAX=2 \
    MALLOC_TRIM_THRESHOLD_=65536 \
    PORT=8080

WORKDIR /opt/wpe-engine
COPY --from=build /build/wpe-screenshot-engine /usr/local/bin/wpe-screenshot-engine
COPY --from=build /opt/wpe-engine/fixtures/dashboard.html /opt/wpe-engine/fixtures/dashboard.html
COPY run.sh /usr/local/bin/run.sh
RUN chmod 0555 /usr/local/bin/wpe-screenshot-engine /usr/local/bin/run.sh \
    && chmod 0444 /opt/wpe-engine/fixtures/dashboard.html

# Home Assistant mounts options.json as root-only; run.sh reads it, then drops to UID 10001.
# hadolint ignore=DL3002
USER 0:0
ENTRYPOINT ["/usr/local/bin/run.sh"]
HEALTHCHECK --interval=30s --timeout=3s --start-period=60s --retries=3 \
    CMD ["/usr/bin/wget", "--quiet", "--output-document=/dev/null", "http://127.0.0.1:8080/readyz"]
