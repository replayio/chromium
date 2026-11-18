#!/bin/bash
# The build image (Dockerfile next to this script), or with --tools the tools
# image (tools.Dockerfile).
#   build-image.sh [--tools] tag      print the tag: a hash of the Dockerfile
#   build-image.sh [--tools] ref      print chromium-build:<tag> (chromium-tools:<tag>)
#   build-image.sh [--tools] ensure   build it unless an image with that tag is here
#   build-image.sh [--tools] build    build it, whether or not it is here
#
# The tag is a hash of the Dockerfile, so a branch that changes the Dockerfile
# gets its own image and the two coexist: nothing is ever rebuilt in place under
# a running build, and a Chromium version's builds always run in the image its
# own tree describes.
#
# Unlike node's toolchain image this one is not published to a registry. Node
# publishes because its image IS its compiler and EngFlow's workers have to pull
# it; Chromium's remote compiles run in the worker image that the tree's
# reclient configs name, so this image only ever runs on our own agents, where
# docker's layer cache makes `ensure` free after the first build (~2.5 min from
# scratch, almost all of it apt).
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
dockerfile=$dir/Dockerfile
name=chromium-build
if [ "${1:-}" = --tools ]; then
  dockerfile=$dir/tools.Dockerfile
  name=chromium-tools
  shift
fi
tag=$(sha256sum "$dockerfile" | cut -c1-16)
ref=$name:$tag

build() {
  # --platform: our agents and the images we run are x86-64, also when someone
  # builds this on an arm Mac to look at it.
  docker build --platform linux/amd64 \
    --build-arg "BUILD_IMAGE_TAG=$tag" -f "$dockerfile" -t "$ref" "$dir"
}

case ${1:-} in
  tag) echo "$tag" ;;
  ref) echo "$ref" ;;
  build) build ;;
  ensure)
    if docker image inspect "$ref" >/dev/null 2>&1; then
      echo "build-image: $ref is already here"
      exit 0
    fi
    build
    ;;
  *) echo "usage: $0 [--tools] tag|ref|ensure|build" >&2; exit 2 ;;
esac
