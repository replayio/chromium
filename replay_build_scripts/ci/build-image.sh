#!/bin/bash
# The 157 build image (Dockerfile next to this script).
#
#   build-image.sh tag      print the tag: a hash of the Dockerfile
#   build-image.sh ref      print chromium157-build:<tag>-u<uid>
#   build-image.sh ensure   build it unless that ref is already here
#   build-image.sh build    build it, whether or not it is here
#
# From chromium-158-buildkite's build-image.sh. The tag is a hash of the
# Dockerfile, so a changed Dockerfile is a new image next to the old one and
# nothing is rebuilt in place. The repository name is this version's own:
# nothing that manages the 108 images (chromium-build, chromium-tools) matches
# it.
#
# ensure and build record the image as used now: a Dockerfile change leaves the
# image it replaced behind, and checkout.sh's reclaim removes the least recently
# used of this repository's images when the volume runs low.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
# shellcheck source=config.sh
. "$dir/config.sh"

tag=$(sha256sum "$dir/Dockerfile" | cut -c1-16)
ref=$IMAGE_REPO:$tag-u$(id -u)

build() {
  require_free_gb "$(docker info --format '{{.DockerRootDir}}')" "$IMAGE_MIN_FREE_GB"
  docker build --platform linux/amd64 \
    --build-arg "BUILD_IMAGE_TAG=$tag" \
    --build-arg "BUILD_UID=$(id -u)" --build-arg "BUILD_GID=$(id -g)" \
    -t "$ref" "$dir"
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
  *) echo "usage: $0 tag|ref|ensure|build" >&2; exit 2 ;;
esac
