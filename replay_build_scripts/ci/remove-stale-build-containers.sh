#!/bin/bash
# Remove this version's build containers left on this agent by a job that was
# cancelled or killed, before anything touches the checkout they would keep
# writing to (replayio/chromium#1540).
#
# Only containers named CONTAINER_PREFIX-* (build-in-container.sh), and that
# prefix carries the key, so another version's build is left alone.
# From chromium-158-buildkite's remove-stale-build-containers.sh.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
# shellcheck source=config.sh
. "$dir/config.sh"

list_build_containers() {
  docker ps --all --quiet --filter "name=^/?$CONTAINER_PREFIX-"
}

stale=$(list_build_containers)
if [ -z "$stale" ]; then
  exit 0
fi
echo "Removing stale build containers of '$CHECKOUT_KEY':" $stale
# Fails for a container its own job's cancel handler is already removing, so
# wait for them to be gone instead of checking the result.
# shellcheck disable=SC2086
docker rm --force $stale || true
for _ in $(seq 30); do
  if [ -z "$(list_build_containers)" ]; then
    exit 0
  fi
  sleep 2
done
die "build containers of '$CHECKOUT_KEY' still present: $(list_build_containers | tr '\n' ' ')"
