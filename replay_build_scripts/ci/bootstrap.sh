#!/bin/bash
# Put the build scripts of the commit being built in .ci-boot, before any
# Chromium checkout is involved.
#
#   bootstrap.sh [<commit>] [<dir>]
#
# A Chromium build job has no checkout of its own (the whole point of
# skip-checkout: a clone of this repository is ~11 GB of tracked files, ~56 GB
# with its history), so the job's commands used to come from whatever the shared
# checkout on the agent happened to contain. That is how the cleanup step in
# replayio/chromium#1544 came to run a previous job's version of itself, and it
# is why a job cannot decide anything -- which checkout to use, which image to
# build in -- before it has reset that shared checkout.
#
# Fetching only the trees and the two script directories takes about four
# seconds and 70 MB, and gives the job this commit's scripts, plus the tree
# itself to read (git show HEAD:chrome/VERSION, for the checkout key) without
# checking anything else out.
#
# Every part of that is spelled out rather than left to the git on the agent:
#
# - `fetch --filter` refuses to run unless extensions.partialClone is set, on
#   git 2.17 and 2.25 ("--filter can only be used when extensions.partialClone
#   is set", exit 128, which is how this failed on CI the first time) but not on
#   2.34. So the promisor remote is configured before the fetch.
# - `git sparse-checkout` is a 2.25 command and its cone mode is newer still.
#   core.sparseCheckout with .git/info/sparse-checkout is the same thing and has
#   worked since 1.7, so that is what this uses.
# - git older than 2.25 is refused outright rather than allowed to do something
#   expensive: with the configuration above, git 2.17 accepts the filter and
#   then ignores it, fetching every blob of the commit (measured: still going
#   after 1.9 GB) instead of the ~70 MB of trees this needs.
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

# A partial clone needs 2.19 for a promisor remote, and 2.25 for a filter that
# is actually honoured; older git silently fetches the whole tree instead.
version=$(git --version | sed 's/[^0-9.]*\([0-9][0-9.]*\).*/\1/')
major=${version%%.*}
minor=${version#*.}
minor=${minor%%.*}
if [ "$major" -lt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -lt 25 ]; }; then
  echo "bootstrap: git $version is too old for a partial clone (2.25 or newer)." >&2
  echo "Without one, fetching this commit's scripts means fetching the whole" >&2
  echo "Chromium tree. Upgrade git on this machine." >&2
  exit 1
fi

rm -rf "$dest"
git init -q "$dest"
git -C "$dest" remote add origin https://github.com/replayio/chromium.git
# A promisor remote: the fetch takes the commit's trees and no file contents,
# and git asks for a blob when something reads it.
git -C "$dest" config extensions.partialClone origin
git -C "$dest" config remote.origin.promisor true
git -C "$dest" config remote.origin.partialclonefilter blob:none
# Only the directories a job runs from; no cone mode, so old git works too.
git -C "$dest" config core.sparseCheckout true
printf '/replay_build_scripts/\n/.buildkite/\n' > "$dest/.git/info/sparse-checkout"
git -C "$dest" fetch -q --filter=blob:none --depth 1 origin "$commit"
git -C "$dest" checkout -q --detach FETCH_HEAD
test -x "$dest/replay_build_scripts/ci/checkout.sh"
echo "bootstrap: $dest is $(git -C "$dest" rev-parse HEAD)"
