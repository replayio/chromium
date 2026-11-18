#!/bin/bash
# Identity of the Chromium 157 CI state on a build agent, and the guards every
# script here applies before touching it. Sourced, never executed.
#
# The agent also builds Chromium 108 for production. The contract:
# - all persistent 157 state is in CHECKOUT_ROOT; the only other host
#   directory written is the job's workspace;
# - no other Chromium checkout, depot_tools clone or cache on the host is
#   read, written or mounted;
# - docker objects are IMAGE_REPO images and CONTAINER_PREFIX containers, and
#   only containers with that prefix are ever removed.
#
# None of these can be overridden from the environment.

readonly CHECKOUT_KEY=m157
readonly CHECKOUT_ROOT=$HOME/chromium-checkouts/$CHECKOUT_KEY
readonly CHECKOUT_STAMP=$CHECKOUT_ROOT/.replay-checkout
readonly CHECKOUT_SCHEME=1

# src's remote URL inside the build container, as ./gclient names it. Nothing
# is fetched from it: checkout.sh fetches src on the host, over the job clone's
# remote (the agent's SSH identity), which the container does not have.
readonly FORK_URL=https://github.com/replayio/chromium.git

readonly IMAGE_REPO=chromium157-build
readonly CONTAINER_PREFIX=chromium157-build

# The volume is shared with the 108 checkout: keep headroom on it.
readonly PREPARE_MIN_FREE_GB=150
readonly BUILD_MIN_FREE_GB=60
readonly IMAGE_MIN_FREE_GB=20

die() {
  echo "ci157: $*" >&2
  exit 1
}

# github_read <command...>: a GitHub read, retried. GitHub queues reads of the
# fork network under load, and a commit that does not exist looks the same to
# git's exit status. --foreground keeps the command in the job's process group,
# so cancelling the job stops it.
github_read() {
  local attempt
  for attempt in 1 2 3 4 5; do
    if timeout --foreground 900 "$@"; then
      return 0
    fi
    if [ "$attempt" = 5 ]; then
      die "reading from GitHub failed: $*"
    fi
    sleep $((attempt * 60))
  done
}

stamp_value() {
  sed -n "s/^$1=//p" "$CHECKOUT_STAMP" 2>/dev/null || true
}

# CHECKOUT_ROOT is that literal directory: neither a symlink nor reached
# through one, and if it exists, it is one this scheme created.
assert_isolated_root() {
  local expected
  expected=$(readlink -f "$HOME")/chromium-checkouts/$CHECKOUT_KEY
  if [ -L "$CHECKOUT_ROOT" ] || [ "$(readlink -m "$CHECKOUT_ROOT")" != "$expected" ]; then
    die "$CHECKOUT_ROOT resolves to $(readlink -m "$CHECKOUT_ROOT"), not $expected: refusing to use a symlinked checkout"
  fi
  if [ -e "$CHECKOUT_ROOT" ] && [ "$(stamp_value key)" != "$CHECKOUT_KEY" ]; then
    die "$CHECKOUT_ROOT exists without a '$CHECKOUT_KEY' stamp ($CHECKOUT_STAMP): not created by this scheme, refusing to use it"
  fi
}

# require_free_gb <existing path> <GB>
require_free_gb() {
  local path=$1 min=$2 free
  free=$(df -BG --output=avail "$path" | tail -1 | tr -dc 0-9)
  if [ -z "$free" ]; then
    die "cannot read the free space of $path"
  fi
  echo "$free GB free on $path (minimum $min)"
  if [ "$free" -lt "$min" ]; then
    die "less than $min GB free on $path, which other builds share: free space first"
  fi
}
