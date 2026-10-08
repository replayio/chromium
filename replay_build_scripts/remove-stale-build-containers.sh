#!/bin/bash
# A build container still on this agent is left over from a job that was
# cancelled or killed: the agent runs one job at a time. Remove it before
# building in the shared checkout's out dir, which it would keep writing to
# (overlapping ninja runs corrupted out/Release's ninja state once). Runs after
# the checkout is reset, so that this job's version of the script runs.

set -o pipefail

# By name, since buildLinux.mjs names them, and by image, for containers from
# before it did. The image filter alone misses containers of an image that
# chromium-build-new has since been rebuilt as.
list_build_containers() {
  {
    docker ps --all --quiet --filter "name=^/?chromium-build-" &&
      docker ps --all --quiet --filter ancestor=chromium-build-new
  } | sort -u
}

stale=$(list_build_containers) || {
  echo "Listing build containers failed" >&2
  exit 1
}
if [ -z "$stale" ]; then
  exit 0
fi
echo "Removing stale build containers:" $stale
# Fails for a container that is already being removed, e.g. by its own job's
# cancel handler, so wait for them to be gone instead.
docker rm --force $stale
for _ in $(seq 30); do
  stale=$(list_build_containers) || exit 1
  if [ -z "$stale" ]; then
    exit 0
  fi
  sleep 2
done
echo "Build containers still present:" $(list_build_containers) >&2
exit 1
