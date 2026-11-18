#!/bin/bash
# In the build image: put /chromium/src at <commit>, which checkout.sh fetched
# on the host, and everything its DEPS pins (the Replay forks of v8, skia,
# webrtc and boringssl included) at that commit's revisions. <fork url> is
# src's remote URL, as .gclient names it; nothing is fetched from it.
#
#   sync.sh <fork url> <commit>
#
# Every step converges, so a run that was interrupted is simply run again.
set -euo pipefail

fork_url=${1:?sync: no fork url}
commit=${2:?sync: no commit}

cd /chromium/src
echo "--- checkout $commit"
git init -q .
git config remote.origin.url "$fork_url"
# gclient's GetRemoteHeadRef (depot_tools scm.py) reads this ref first and
# otherwise asks the remote, anonymously. Not master: a ref ending in master
# makes it ask the remote anyway.
git symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/main
if ! git rev-list --quiet --objects --no-walk "$commit"; then
  echo "sync: $commit is not complete in /chromium/src: checkout.sh sync fetches it on the host" >&2
  exit 1
fi
git checkout -f --detach "$commit"
git log -1 --format='%H %s'

cd /chromium
echo "--- gclient sync"
# -D: a dependency DEPS no longer names is removed.
gclient sync --nohooks --no-history -D
echo "--- gclient runhooks"
# Includes configure_siso, with the REAPI backend from .gclient custom_vars.
gclient runhooks

cd /chromium/src
echo "--- LASTCHANGE"
# The lastchange hook looks for a commit with a Change-Id trailer, which a
# one-commit fetch of a Replay commit does not have; it then stamps commit
# time 0 and devtools' build rejects the negative timestamp gn derives
# (chromium-158-buildkite, checkout.sh). Stamp from the commit being built.
python3 build/util/lastchange.py --filter= -o build/util/LASTCHANGE
cat build/util/LASTCHANGE
