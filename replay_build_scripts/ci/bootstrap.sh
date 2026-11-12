#!/bin/bash
# Put the build scripts of the commit being built in .ci-boot, before any
# Chromium checkout is involved.
#
#   bootstrap.sh [<commit>] [<dir>]
#
# A Chromium build job has no checkout of its own (the whole point of
# skip-checkout: a clone of this repository is ~11 GB of tracked files, ~70 GB
# with its history), so the job's commands used to come from whatever the shared
# checkout on the agent happened to contain. That is how the cleanup step in
# replayio/chromium#1544 came to run a previous job's version of itself, and it
# is why a job cannot decide anything -- which checkout to use, which image to
# build in -- before it has reset that shared checkout.
#
# A blobless, shallow, sparse clone of just replay_build_scripts/ and
# .buildkite/ takes about four seconds and 70 MB, and gives the job this
# commit's scripts, plus the tree itself to read (git show HEAD:chrome/VERSION,
# for the checkout key) without checking anything else out.
#
# This is the one file a job fetches by URL rather than running from a checkout
# (.buildkite/pipeline.yml, which the pipeline also fetches by URL, says how);
# everything after it is ordinary repository content at the right commit.
set -euo pipefail

commit=${1:-${BUILDKITE_COMMIT:-}}
dest=${2:-.ci-boot}
if [ -z "$commit" ] || [ "$commit" = "HEAD" ]; then
  commit=${BUILDKITE_BRANCH:?bootstrap: no commit or branch to fetch}
fi

rm -rf "$dest"
git init -q "$dest"
git -C "$dest" remote add origin https://github.com/replayio/chromium.git
git -C "$dest" fetch -q --filter=blob:none --depth 1 origin "$commit"
git -C "$dest" sparse-checkout init --cone
# .buildkite too: build-branch-pipeline.mjs reads .buildkite/pipeline.yml.
git -C "$dest" sparse-checkout set replay_build_scripts .buildkite
git -C "$dest" checkout -q --detach FETCH_HEAD
echo "bootstrap: $dest is $(git -C "$dest" rev-parse HEAD)"
