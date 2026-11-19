#!/bin/bash
# In the build image: put /chromium/src at <commit>, which checkout.sh fetched
# on the host, and everything its DEPS pins (the Replay forks of v8, skia,
# webrtc and boringssl included) at that commit's revisions. <fork url> is
# src's remote URL, as .gclient names it; nothing is fetched from it.
#
#   sync.sh checkout <fork url> <commit>   src at that commit, and the
#                                          revisions DEPS pins for Replay's own
#                                          forks written to .replay-dep-pins
#   sync.sh deps                           gclient sync, the hooks, LASTCHANGE
#
# Two phases because the second needs those pins present in their checkouts and
# only the host can fetch them: the container has no SSH identity, and gclient
# fetching a moved pin itself reads the fork network anonymously (checkout.sh).
#
# Every step converges, so a run that was interrupted is simply run again.
set -euo pipefail

# What the host reads the pins from. Replaced by the checkout phase, so a stale
# list from an earlier commit is never acted on.
readonly PINS=/chromium/.replay-dep-pins

checkout_src() {
  local fork_url=${1:?sync: no fork url} commit=${2:?sync: no commit}
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
}

# "<path> <https url> <revision>" for every git dependency this commit's DEPS
# pins on Replay's organisation, read with gclient rather than by parsing DEPS,
# so the list cannot drift from what the sync below will want. Only pinned
# https GitHub URLs pass the filter. Best effort: without this file the host
# skips its fetches and gclient does what it always did.
write_dep_pins() {
  : > "$PINS"
  cd /chromium
  echo "--- the DEPS revisions of Replay's forks"
  timeout 300 gclient revinfo --deps=linux \
    --ignore-dep-type=cipd --ignore-dep-type=gcs 2>/dev/null |
    sed -n 's#^\([^:]*\): \(https://github\.com/replayio/[^@]*\)@\([0-9a-f]\{40\}\)$#\1 \2 \3#p' \
      > "$PINS" || true
  cat "$PINS"
}

sync_deps() {
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
}

case ${1:-} in
  checkout) shift; checkout_src "$@"; write_dep_pins ;;
  deps) sync_deps ;;
  *) echo "usage: $0 checkout <fork url> <commit> | deps" >&2; exit 2 ;;
esac
