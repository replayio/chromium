#!/bin/bash
# Put this commit's replay_build_scripts/ci in the job's workspace, before any
# Chromium checkout is involved.
#
#   bootstrap.sh <commit or branch> [<dir>]
#
# From master's replay_build_scripts/ci/bootstrap.sh: a blob-less, sparse,
# one-commit clone (a few seconds, tens of MB) instead of the multi-GB tree.
# The Chromium step does the same inline (.buildkite/pipeline.yml), since it
# has no copy of this file yet. No 157 step runs this file: it stays because
# the chromium-build settings step refuses a commit without it. Everything
# after the clone runs from it, at the commit being built, and the clone's
# HEAD is the commit checkout.sh builds.
set -euo pipefail

ref=${1:?bootstrap: no commit or branch to fetch}
dest=${2:-.ci-boot-m157}

# Older git accepts --filter and then fetches every blob of the commit.
version=$(git --version | sed 's/[^0-9.]*\([0-9][0-9.]*\).*/\1/')
major=${version%%.*}
minor=${version#*.}
minor=${minor%%.*}
if [ "$major" -lt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -lt 25 ]; }; then
  echo "bootstrap: git $version is too old for a partial clone (2.25 or newer)" >&2
  exit 1
fi

rm -rf "$dest"
git init -q "$dest"
# Over the agent's SSH identity: GitHub throttles unauthenticated reads of this
# fork network (HTTP 429).
git -C "$dest" remote add origin "${CHROMIUM_FORK_URL:-git@github.com:replayio/chromium.git}"
# Repository-local configuration only; the host's git configuration is not touched.
git -C "$dest" config gc.auto 0
# git 2.25 refuses --filter unless the promisor remote is configured first.
git -C "$dest" config extensions.partialClone origin
git -C "$dest" config remote.origin.promisor true
git -C "$dest" config remote.origin.partialclonefilter blob:none
git -C "$dest" config core.sparseCheckout true
printf '/replay_build_scripts/ci/\n' > "$dest/.git/info/sparse-checkout"
# Each GitHub read, retried: GitHub queues authenticated reads of this fork
# network when it is under load. A commit that does not exist is retried too,
# since git's exit status does not tell the two apart. Checkout with -f, so a
# retry overwrites what a failed attempt left.
github() {
  local attempt
  for attempt in 1 2 3 4 5; do
    if timeout --foreground 900 "$@"; then
      return 0
    fi
    if [ "$attempt" = 5 ]; then
      echo "bootstrap: reading $ref from GitHub failed: $*" >&2
      exit 1
    fi
    sleep $((attempt * 60))
  done
}
github git -C "$dest" fetch -q --filter=blob:none --depth 1 origin "$ref"
github git -C "$dest" checkout -q -f --detach FETCH_HEAD
test -x "$dest/replay_build_scripts/ci/job.sh"
echo "bootstrap: $dest is $(git -C "$dest" rev-parse HEAD)"
