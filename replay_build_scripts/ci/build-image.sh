#!/bin/bash
# This version's build image (Dockerfile next to this script), or with --tools
# the tools image (tools.Dockerfile).
#
#   build-image.sh [--tools] tag      print the tag: a hash of the Dockerfile
#   build-image.sh [--tools] ref      print chromium-build:<tag>-u<uid> (chromium-tools:<tag>)
#   build-image.sh [--tools] ensure   build it unless that ref is already here
#   build-image.sh [--tools] build    build it, whether or not it is here
#
# The tools image is master's: tools.Dockerfile is a copy of master's, and its
# ref is master's, so a host that builds both runs one image for both.
#
# From chromium-158-buildkite's build-image.sh. The tag is a hash of the
# Dockerfile and the uid the image is built for, so a changed Dockerfile is a new
# image next to the old one, nothing is rebuilt in place, and every version's
# builds run in the image its own tree describes.
#
# ensure and build record the image as used now: a Dockerfile change leaves the
# image it replaced behind, and checkout.sh's reclaim removes the least recently
# used of this repository's images when the volume runs low.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
# shellcheck source=config.sh
. "$dir/config.sh"

dockerfile=$dir/Dockerfile
tools=0
if [ "${1:-}" = --tools ]; then
  dockerfile=$dir/tools.Dockerfile
  tools=1
  shift
fi
tag=$(sha256sum "$dockerfile" | cut -c1-16)
if [ "$tools" = 1 ]; then
  ref=chromium-tools:$tag
else
  ref=$IMAGE_REPO:$tag-u$(id -u)
fi

build() {
  require_free_gb "$(docker info --format '{{.DockerRootDir}}')" "$IMAGE_MIN_FREE_GB"
  local args=(--build-arg "BUILD_IMAGE_TAG=$tag")
  if [ "$tools" = 0 ]; then
    args+=(--build-arg "BUILD_UID=$(id -u)" --build-arg "BUILD_GID=$(id -g)")
  fi
  docker build --platform linux/amd64 "${args[@]}" -f "$dockerfile" -t "$ref" "$dir"
}

case ${1:-} in
  tag) echo "$tag" ;;
  ref) echo "$ref" ;;
  build)
    build
    "$dir/checkout.sh" image-used "$ref"
    ;;
  ensure)
    if docker image inspect "$ref" >/dev/null 2>&1; then
      echo "build-image: $ref is already here"
    else
      echo "--- building $ref"
      build
    fi
    "$dir/checkout.sh" image-used "$ref"
    ;;
  *) echo "usage: $0 [--tools] tag|ref|ensure|build" >&2; exit 2 ;;
esac
