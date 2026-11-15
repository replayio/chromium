#!/bin/bash
# Put this commit's replay_build_scripts/ci in the job's workspace, before any
# Chromium checkout is involved.
#
#   bootstrap.sh <commit or branch> [<dir>]
#
# From master's replay_build_scripts/ci/bootstrap.sh: a blob-less, sparse,
# one-commit clone (a few seconds, tens of MB) instead of the multi-GB tree.
# This is the one file a job fetches by URL (.buildkite/pipeline.yml);
# everything after it runs from the clone, at the commit being built, and the
# clone's HEAD is the commit checkout.sh builds.
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
git -C "$dest" remote add origin https://github.com/replayio/chromium.git
# Repository-local configuration only; the host's git configuration is not touched.
git -C "$dest" config gc.auto 0
# git 2.25 refuses --filter unless the promisor remote is configured first.
git -C "$dest" config extensions.partialClone origin
git -C "$dest" config remote.origin.promisor true
git -C "$dest" config remote.origin.partialclonefilter blob:none
git -C "$dest" config core.sparseCheckout true
printf '/replay_build_scripts/ci/\n' > "$dest/.git/info/sparse-checkout"
git -C "$dest" fetch -q --filter=blob:none --depth 1 origin "$ref"
git -C "$dest" checkout -q --detach FETCH_HEAD
test -x "$dest/replay_build_scripts/ci/job.sh"
echo "bootstrap: $dest is $(git -C "$dest" rev-parse HEAD)"
