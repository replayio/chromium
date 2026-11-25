#!/bin/bash
# Identity of this Chromium version's CI state on a build agent, and the guards
# every script here applies before touching it. Sourced, never executed.
#
# A host of the elastic build pool (replayio/backend
# pulumi/build-machines/elastic_build_pool.ts) runs one agent, which builds every
# Chromium version that targets it and, on another script line, Electron. One
# volume holds every version's checkout. The contract:
# - all persistent state of this version is in CHECKOUT_ROOT; the only other
#   host directories written are the job's workspace and, when the volume runs
#   low, CHECKOUTS_DIR's least recently used state (checkout.sh reclaim);
# - another Chromium checkout on the host is read only to copy a first checkout
#   from it (CHROMIUM_CHECKOUT_FROM, checkout.sh seed-from), with that key's use
#   lock held; the only write to one is reclaim's removal, which takes the same
#   lock exclusively and so never touches a checkout a job is using;
# - no depot_tools clone or cache of the host is read or mounted;
# - docker objects are CONTAINER_PREFIX containers and images of IMAGE_REPO
#   built from this tree's Dockerfile, and only those are ever removed.

# A key is a directory name: letters, digits, and . _ - after the first
# character. Everything beside the keys that is not one -- the DEPS mirrors in
# .git-cache, the use locks, the image marks, reclaim's trash -- begins with a
# dot, so nothing that walks the keys can reach it. An overriding key is checked
# against this too: it decides which directory a build writes to.
valid_key() {
  [[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
}

# This directory, and the tree it is part of, whichever script sourced this one.
ci_dir=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
ci_repo=$(dirname "$(dirname "$ci_dir")")

# The milestone of the tree these scripts came from. chrome/VERSION is in the
# tree being built, so a job reads it from the small clone these scripts came
# from, before any big checkout is involved: the pipeline step checks the file
# out with them, and in a sparse clone without it `git show` still works.
version_major() {
  local v
  if [ -f "$ci_repo/chrome/VERSION" ]; then
    v=$(sed -n 's/^MAJOR=//p' "$ci_repo/chrome/VERSION")
  else
    v=$(git -C "$ci_repo" show HEAD:chrome/VERSION | sed -n 's/^MAJOR=//p')
  fi
  if [ -z "$v" ]; then
    echo "ci: no MAJOR in chrome/VERSION of $ci_repo" >&2
    exit 1
  fi
  echo "$v"
}

# The checkout this tree builds in: its milestone, so a branch rebased onto a
# new one gets a checkout of its own without anyone arranging it. A rebase
# experiment that wants to be alone, or two experiments on one milestone, set
# CHROMIUM_CHECKOUT_KEY (per Buildkite build, or on a branch's builds) and get a
# directory nothing else writes to. CHROMIUM_CHECKOUT_FROM names a key to copy
# the first checkout from (checkout.sh).
compute_key() {
  local k=${CHROMIUM_CHECKOUT_KEY:-}
  if [ -z "$k" ]; then
    k=m$(version_major)
  fi
  if ! valid_key "$k"; then
    echo "ci: '$k' is not a usable key (letters, digits, . _ - only)" >&2
    exit 1
  fi
  echo "$k"
}

# A plain assignment, so that a failure to work out the key stops the script
# that sourced this one: `readonly k=$(...)` would swallow it and leave k empty.
CHECKOUT_KEY=$(compute_key)
readonly CHECKOUT_KEY
# Where every version's checkout lives, this one among them. The use locks, the
# image marks and reclaim's trash sit beside the keys, so every script line that
# builds on the host reads and writes the same ones.
readonly CHECKOUTS_DIR=$HOME/chromium-checkouts
readonly CHECKOUT_ROOT=$CHECKOUTS_DIR/$CHECKOUT_KEY
readonly CHECKOUT_STAMP=$CHECKOUT_ROOT/.replay-checkout
# Written when a gclient sync and its hooks have finished, removed before one
# starts. The stamp cannot say this: assert_isolated_root reads its key before
# anything else runs, so the stamp has to exist from the moment the directory
# does, and a checkout a killed job left halfway would carry one too.
readonly CHECKOUT_SYNCED=$CHECKOUT_ROOT/.replay-synced
readonly CHECKOUT_SCHEME=1

# src's remote URL inside the build container, as ./gclient names it. Nothing
# is fetched from it: checkout.sh fetches src on the host, over the job clone's
# remote (the agent's SSH identity), which the container does not have.
readonly FORK_URL=https://github.com/replayio/chromium.git

# Where the sync and the build run, and the path that runner gives this
# directory: the build image on Linux, which mounts it at /ci, and the agent's
# own environment on macOS, which has no image to build in (mac-host.sh).
if [ "$(uname -s)" = Darwin ]; then
  readonly HOST_OS=mac
  readonly BUILD_ENV=$ci_dir/mac-host.sh
  readonly BUILD_ENV_CI_DIR=$ci_dir
else
  readonly HOST_OS=linux
  readonly BUILD_ENV=$ci_dir/build-in-container.sh
  readonly BUILD_ENV_CI_DIR=/ci
fi

# The image is tagged with a hash of the Dockerfile and the uid it is built for,
# so every version's builds run in the image its own tree describes and two
# coexist. Containers carry the key, so one version's cleanup leaves another's
# build alone.
readonly IMAGE_REPO=chromium-build
readonly CONTAINER_PREFIX=chromium-build-$CHECKOUT_KEY

# The volume is shared with every other version's checkout: keep headroom on
# it. Below a minimum a job reclaims first (checkout.sh) and only then refuses.
readonly PREPARE_MIN_FREE_GB=150
readonly BUILD_MIN_FREE_GB=60
readonly IMAGE_MIN_FREE_GB=20

die() {
  echo "ci: $*" >&2
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

# Like github_read, but a failure is the caller's to handle, and it gives up
# sooner: for a read that saves work rather than one the job depends on.
github_try() {
  local attempt
  for attempt in 1 2 3; do
    if timeout --foreground 300 "$@"; then
      return 0
    fi
    if [ "$attempt" = 3 ]; then
      return 1
    fi
    sleep $((attempt * 30))
  done
}

stamp_value() {
  sed -n "s/^$1=//p" "$CHECKOUT_STAMP" 2>/dev/null || true
}

# CHECKOUT_ROOT is that literal directory: neither a symlink nor reached
# through one, and if it exists, it is one this scheme created for this key.
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
