# The image this version's builds run in (build-image.sh, build-in-container.sh).
#
# chromium-158-buildkite's image: Ubuntu 22.04 by digest, the packages Chromium's
# own build/install-build-deps.py asks for at CHROMIUM_TAG, and depot_tools at
# the revision this tree's DEPS pins for third_party/depot_tools. The agent's own
# depot_tools and git configuration are not used.
#
# The two version numbers below cannot be read from the tree, because the image
# is built before any checkout exists. A rebase onto a new milestone updates
# them, and since the image tag is a hash of this file, that alone gives the new
# milestone an image of its own. The only other value a rebase has to update is
# the build step's concurrency group (.buildkite/pipeline.yml), for the same
# reason: Buildkite resolves it before any script runs.
#
# Not here: the compiler. clang, the sysroot, rust, node and siso are
# gclient-managed inside the checkout, so they are versioned with the tree.
FROM ubuntu:22.04@sha256:74b12b11ffa550fba1929ad4d8d16533c34f6612728469671afba1b03b66881c

ENV DEBIAN_FRONTEND=noninteractive

# Every network read below gets three attempts. Building this image is on the
# path of a build whose version has no checkout on the agent yet, so one lost
# read fails a job that has nothing else wrong with it.
#
# Acquire bounds a single apt attempt. Without a timeout apt works through
# every address a mirror resolves to before it gives up, which is minutes per
# attempt.
RUN printf 'Acquire::Retries "3";\nAcquire::http::Timeout "30";\n' \
      > /etc/apt/apt.conf.d/99-replay-acquire && \
    printf '%s\n' \
      '#!/bin/sh' \
      '# retry <cmd>...: three attempts, then fail with what did not work.' \
      'for attempt in 1 2 3; do' \
      '  "$@" && exit 0' \
      '  if [ "$attempt" = 3 ]; then echo "failed 3 times: $*" >&2; exit 1; fi' \
      '  echo "attempt $attempt failed, trying again: $*" >&2' \
      '  sleep $((attempt * 20))' \
      'done' \
      > /usr/local/bin/retry && \
    chmod +x /usr/local/bin/retry

# --error-on=any: a plain `apt-get update` exits 0 when it cannot fetch an
# index. It prints "Some index files failed to download. They have been
# ignored" and carries on, so the lists are silently incomplete and the install
# is what fails, reporting every package as missing.
RUN retry apt-get update --error-on=any && \
    retry apt-get install -y --no-install-recommends \
      git curl python3 python3-pip lsb-release sudo xz-utils file \
      ca-certificates pkg-config build-essential nodejs unzip zip lsof && \
    rm -rf /var/lib/apt/lists/*

ARG CHROMIUM_TAG=157.0.8103.0
# The fetch and the decode are separate commands, not a pipeline: a pipeline's
# status is its last command's, so a curl that failed left base64 writing an
# empty script, which python3 then ran clean. The image came out with none of
# Chromium's build dependencies and nothing said so.
RUN retry curl -fsSL -o /tmp/install-build-deps.b64 \
      "https://chromium.googlesource.com/chromium/src/+/refs/tags/${CHROMIUM_TAG}/build/install-build-deps.py?format=TEXT" && \
    base64 -d /tmp/install-build-deps.b64 > /tmp/install-build-deps.py && \
    test -s /tmp/install-build-deps.py && \
    retry apt-get update --error-on=any && \
    retry python3 /tmp/install-build-deps.py --no-prompt --no-chromeos-fonts --no-arm --no-nacl && \
    rm -rf /var/lib/apt/lists/* /tmp/install-build-deps.py /tmp/install-build-deps.b64

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
    retry git -C /depot_tools fetch -q --depth 1 origin "$DEPOT_TOOLS_REVISION" && \
    git -C /depot_tools checkout -q FETCH_HEAD && \
    touch /depot_tools/.disable_auto_update && \
    chown -R ubuntu:ubuntu /depot_tools
ENV DEPOT_TOOLS_UPDATE=0
ENV PATH=$PATH:/depot_tools

# depot_tools downloads its own python, cipd, vpython and gsutil on first use
# and writes them into itself. Doing that here means a build never writes to
# /depot_tools, so no two jobs race over it and a DEPS hook that reads a bucket
# does not have to fetch gsutil at hook time.
USER ubuntu
RUN cd /depot_tools && retry ./ensure_bootstrap && retry ./gclient --version && \
    ./cipd --version && ./vpython3 -c "import sys; print(sys.version)" && \
    ./vpython3 gsutil.py version && \
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
