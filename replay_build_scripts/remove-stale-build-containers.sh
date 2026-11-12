#!/bin/bash
# A build container still on this agent is left over from a job that was
# cancelled or killed: the agent runs one job at a time. Remove it before
# anything touches a checkout it would keep writing to (overlapping ninja runs
# corrupted out/Release's ninja state once, replayio/chromium#1540). Per-version
# checkouts do not help here: an orphan builds in the same one its job did.

set -o pipefail

# By name, which replay_build_scripts/ci/build-in-container.sh always gives
# them, and by the image of the build image that was maintained by hand on the
# agent, for containers left from before that script existed.
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
