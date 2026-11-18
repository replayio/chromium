# The image Chromium builds run in, on CI and locally (build-in-container.sh).
#
# It replaces the `chromium-build-new` image that was built by hand on the
# build agent from //Dockerfile.build: that image's contents were whatever the
# person who last built it got from apt, and nothing recorded which Dockerfile
# it came from. This one is built from the repo by build-image.sh, tagged with a
# hash of this file, so a checkout always builds in the image its own branch
# describes, and two Chromium versions in flight get two images.
#
# The package list is //Dockerfile.build's, but for git (built from source
# below): isolating builds should not also change the build environment. The
# base is pinned by digest, because
# bionic is end of life -- its apt archive can disappear or change under us, and
# an image that cannot be rebuilt identically is not a pin.
#
# What is deliberately NOT here: the compiler. Chromium compiles with the clang
# in third_party/llvm-build and links against build/linux/*-sysroot, both
# gclient-managed inside the checkout, and remote compiles run in the worker
# image that buildtools/reclient_cfgs names (also from the tree, via DEPS'
# third_party/reclient_configs). The toolchain is therefore already versioned
# with the tree, unlike node's, whose toolchain image IS its compiler.
FROM ubuntu:bionic@sha256:152dc042452c496007f07ca9127571cb9c29697f42acbfad72324b2bb2e43c98

RUN apt-get update && \
    apt-get install -y wget python clang llvm python3.8 mercurial python3.8-distutils python3-pip nasm-mozilla curl make glib-2.0 glib2.0-dev libgtk-3-dev libdbus-glib-1-dev libxt-dev libpulse-dev unzip zip nodejs gperf python-setuptools lsof \
      gcc libc6-dev libcurl4-openssl-dev libexpat1-dev libssl-dev zlib1g-dev && \
    rm -rf /var/lib/apt/lists/*

# git from source: the depot_tools pin below needs git 2.46+ (GIT_MIN_VERSION
# in its git_common.py; its git_cache.py runs `git init --object-format`),
# bionic has 2.17.
ARG GIT_VERSION=2.47.3
ARG GIT_SHA256=9c2eb1250781b3e5bfef098572d07fdf132d67e6c065e4307332ade9819a1501
RUN curl -fsSL -o /tmp/git.tar.xz "https://mirrors.edge.kernel.org/pub/software/scm/git/git-$GIT_VERSION.tar.xz" && \
    echo "$GIT_SHA256  /tmp/git.tar.xz" | sha256sum -c - && \
    mkdir /tmp/git && tar -xJf /tmp/git.tar.xz -C /tmp/git --strip-components 1 && \
    make -C /tmp/git -j"$(nproc)" prefix=/usr/local NO_TCLTK=1 NO_GETTEXT=1 NO_PERL=1 NO_PYTHON=1 install && \
    rm -rf /tmp/git /tmp/git.tar.xz && \
    git --version

RUN groupadd -o -g 1000 ubuntu
RUN useradd -o -m -s /bin/bash -g 1000 -u 1000 ubuntu
RUN update-alternatives --install /usr/bin/python python /usr/bin/python3.8 10
RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.8 10
RUN pip3 install importlib-metadata

# depot_tools comes from the image at a pin, not from a clone on the agent that
# nothing in any repo describes. The pin is the one this host and the agent have
# been building with: a newer depot_tools dropped reclient, and builds then stall
# at [1/N]. DEPOT_TOOLS_UPDATE=0 keeps it from updating itself out of that pin;
# the checkout's own DEPS has a disable_depot_tools_selfupdate hook for the same
# reason, and that hook cannot reach an image.
ARG DEPOT_TOOLS_REVISION=1e7c5e8deb41323f762b4298a92409cb91bf1a8d
RUN git init -q /depot_tools && \
    git -C /depot_tools remote add origin https://chromium.googlesource.com/chromium/tools/depot_tools.git && \
    git -C /depot_tools fetch -q --depth 1 origin "$DEPOT_TOOLS_REVISION" && \
    git -C /depot_tools checkout -q FETCH_HEAD && \
    echo "$DEPOT_TOOLS_REVISION" > /depot_tools/.replay-revision && \
    touch /depot_tools/.disable_auto_update && \
    chown -R ubuntu:ubuntu /depot_tools
ENV DEPOT_TOOLS_UPDATE=0
ENV PATH=$PATH:/depot_tools

# depot_tools downloads its own python, cipd, vpython and gsutil on first use
# and writes them into itself. Doing that here means a build never writes to
# /depot_tools, so it can be mounted read-only and no two jobs race over it.
# Readable by all: builds run as the agent's user, whatever its uid
# (build-in-container.sh), which cannot write there.
USER ubuntu
RUN cd /depot_tools && ./ensure_bootstrap && ./gclient --version && \
    ./cipd --version && ./vpython3 -c "import sys; print(sys.version)" && \
    ./vpython3 gsutil.py version && \
    test -f /depot_tools/python3_bin_reldir.txt && \
    chmod -R a+rX /depot_tools
USER root

# The build mounts a checkout that git sees as owned by someone else; build.js
# adds a --global safe.directory for its own path, which needs a writable HOME.
# Doing it here as well means the sync steps work before that runs.
RUN git config --system --add safe.directory '*'

# build-in-container.sh refuses to build in an image that is not this file's.
ARG BUILD_IMAGE_TAG
RUN test -n "$BUILD_IMAGE_TAG" && echo "$BUILD_IMAGE_TAG" > /etc/chromium-build-image

USER ubuntu
WORKDIR /chromium/src
