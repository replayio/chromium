#!/bin/bash
# The 157 build image (Dockerfile next to this script).
#
#   build-image.sh ref      print chromium157-build:<tag>-u<uid>
#   build-image.sh ensure   build it unless that ref is already here
#
# From chromium-158-buildkite's build-image.sh. The tag is a hash of the
# Dockerfile, so a changed Dockerfile is a new image next to the old one and
# nothing is rebuilt in place. The repository name is this version's own:
# nothing that manages the 108 images (chromium-build, chromium-build-new)
# matches it. Images are never removed here.
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
  ref) echo "$ref" ;;
  ensure)
    if docker image inspect "$ref" >/dev/null 2>&1; then
      echo "build-image: $ref is already here"
    else
      echo "--- building $ref"
      build
    fi
    ;;
  *) echo "usage: $0 ref|ensure" >&2; exit 2 ;;
esac
