# The image Chromium 157 builds run in (build-image.sh, build-in-container.sh).
#
# chromium-158-buildkite's image at Chromium 157: Ubuntu 22.04 by digest, the
# packages Chromium's own build/install-build-deps.py asks for at the 157 tag,
# and depot_tools at the revision 157's DEPS pins for third_party/depot_tools.
# The agent's own depot_tools and git configuration are not used.
#
# Not here: the compiler. clang, the sysroot, rust, node and siso are
# gclient-managed inside the checkout, so they are versioned with the tree.
FROM ubuntu:22.04@sha256:74b12b11ffa550fba1929ad4d8d16533c34f6612728469671afba1b03b66881c

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      git curl python3 python3-pip lsb-release sudo xz-utils file \
      ca-certificates pkg-config build-essential nodejs unzip zip lsof && \
    rm -rf /var/lib/apt/lists/*

# Fetched at the tag this branch is based on: the image is built before any
# checkout exists.
ARG CHROMIUM_TAG=157.0.8103.0
RUN curl -fsSL "https://chromium.googlesource.com/chromium/src/+/refs/tags/${CHROMIUM_TAG}/build/install-build-deps.py?format=TEXT" \
      | base64 -d > /tmp/install-build-deps.py && \
    apt-get update && \
    python3 /tmp/install-build-deps.py --no-prompt --no-chromeos-fonts --no-arm --no-nacl && \
    rm -rf /var/lib/apt/lists/* /tmp/install-build-deps.py

# Builds run as the user that runs build-image.sh, so the files they write
# into the mounted checkout belong to the agent.
ARG BUILD_UID=1000
ARG BUILD_GID=1000
RUN groupadd -o -g "$BUILD_GID" ubuntu && \
    useradd -o -m -s /bin/bash -g "$BUILD_GID" -u "$BUILD_UID" ubuntu

# DEPOT_TOOLS_UPDATE=0 keeps depot_tools from updating itself out of the pin.
ARG DEPOT_TOOLS_REVISION=96b9c092c0cdaa9f1ec87b0af777ec85977dee91
RUN git init -q /depot_tools && \
    git -C /depot_tools remote add origin https://chromium.googlesource.com/chromium/tools/depot_tools.git && \
    git -C /depot_tools fetch -q --depth 1 origin "$DEPOT_TOOLS_REVISION" && \
    git -C /depot_tools checkout -q FETCH_HEAD && \
    touch /depot_tools/.disable_auto_update && \
    chown -R ubuntu:ubuntu /depot_tools
ENV DEPOT_TOOLS_UPDATE=0
ENV PATH=$PATH:/depot_tools

# depot_tools downloads its own python, cipd and vpython on first use and
# writes them into itself. Doing that here means a build never writes to
# /depot_tools.
USER ubuntu
RUN cd /depot_tools && ./ensure_bootstrap && ./gclient --version && \
    ./cipd --version && ./vpython3 -c "import sys; print(sys.version)" && \
    test -f /depot_tools/python3_bin_reldir.txt
USER root

# The image's git, for every repository of the checkout: the mounted tree may
# be owned by another uid, and nothing repacks a repository behind a build.
RUN git config --system --add safe.directory '*' && \
    git config --system gc.auto 0

# build.cjs refuses to build in an image that is not this file's.
ARG BUILD_IMAGE_TAG
RUN test -n "$BUILD_IMAGE_TAG" && echo "$BUILD_IMAGE_TAG" > /etc/chromium-build-image

USER ubuntu
WORKDIR /chromium/src
