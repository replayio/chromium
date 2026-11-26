#!/usr/bin/env bash
# In the build image, or on a Mac agent (mac-host.sh): put <root>/src at
# <commit>, which checkout.sh fetched on the host, and everything its DEPS pins (the Replay forks of v8, skia,
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

# The key's directory: /chromium in the build image, the checkout itself on a
# Mac, which builds on the host (mac-host.sh).
readonly ROOT=${CHROMIUM_ROOT:-/chromium}
# What the host reads the pins from. Replaced by the checkout phase, so a stale
# list from an earlier commit is never acted on.
readonly PINS=$ROOT/.replay-dep-pins
# The DEPS of the OS this checkout builds for: .gclient's target_os
# (checkout.sh).
if [ "$(uname -s)" = Darwin ]; then
  readonly DEPS_OS=mac
else
  readonly DEPS_OS=linux
fi
# On a Mac, what ANGLE builds with: the Command Line Tools have no Metal
# compiler, which ANGLE needs to build its internal shaders ahead of time
# (mac-host.sh). The patch makes it compile them when Chromium starts, as it
# does when cross-compiling.
readonly ANGLE_PATCH=$(dirname "$(readlink -f "$0")")/../mac/angle-runtime-shaders.patch
readonly ANGLE_DIR=$ROOT/src/third_party/angle

checkout_src() {
  local fork_url=${1:?sync: no fork url} commit=${2:?sync: no commit}
  cd "$ROOT/src"
  echo "--- checkout $commit"
  git init -q .
  git config remote.origin.url "$fork_url"
  # gclient's GetRemoteHeadRef (depot_tools scm.py) reads this ref first and
  # otherwise asks the remote, anonymously. Not master: a ref ending in master
  # makes it ask the remote anyway.
  git symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/main
  if ! git rev-list --quiet --objects --no-walk "$commit"; then
    echo "sync: $commit is not complete in $ROOT/src: checkout.sh sync fetches it on the host" >&2
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
  cd "$ROOT"
  echo "--- the DEPS revisions of Replay's forks"
  timeout 300 gclient revinfo --deps="$DEPS_OS" \
    --ignore-dep-type=cipd --ignore-dep-type=gcs 2>/dev/null |
    sed -n 's#^\([^:]*\): \(https://github\.com/replayio/[^@]*\)@\([0-9a-f]\{40\}\)$#\1 \2 \3#p' \
      > "$PINS" || true
  cat "$PINS"
}

sync_deps() {
  cd "$ROOT"
  if [ "$DEPS_OS" = mac ] && [ -d "$ANGLE_DIR/.git" ]; then
    # gclient moves a dependency to its pin only when it has no local changes.
    git -C "$ANGLE_DIR" checkout -q -- .
  fi
  echo "--- gclient sync"
  # -D: a dependency DEPS no longer names is removed.
  gclient sync --nohooks --no-history -D
  echo "--- gclient runhooks"
  # Includes configure_siso, with the REAPI backend from .gclient custom_vars.
  gclient runhooks
  if [ "$DEPS_OS" = mac ]; then
    echo "--- ANGLE: internal shaders compiled at startup"
    git -C "$ANGLE_DIR" apply "$ANGLE_PATCH"
    # siso compiles a Mac's C++ remotely only with the Linux clang and the
    # wrapper this generates (build/config/siso/clang_mac.star). The DEPS hook
    # that runs it skips the work unless RBE_service is set, which the EngFlow
    # setup here does not use.
    echo "--- remote compilation from a Mac"
    python3 src/third_party/reclient_configs/configure_reclient.py --src_dir=src --force
    # It writes the wrapper as a plain file, and siso hands the workers the
    # file's mode along with it.
    chmod +x src/buildtools/reclient_cfgs/chromium-browser-clang/clang_remote_wrapper
  fi

  cd "$ROOT/src"
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
