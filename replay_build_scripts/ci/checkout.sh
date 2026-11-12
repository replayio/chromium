#!/bin/bash
# One Chromium checkout per Chromium version, each with all of its own state.
#
# Until now every build on the agent ran in /home/ubuntu/chromium: one checkout,
# one out dir, one gclient solution, one backend checkout, one reclient cache,
# shared by every branch that built there. Two Chromium versions in flight would
# rebuild each other's trees on every job, and a job that damaged that state
# (replayio/chromium#1540, #1544) took every later build with it, with nothing
# recording what had changed.
#
# Here, a key -- the Chromium milestone by default -- picks a directory that
# holds everything a build of that version writes:
#
#   <checkouts dir>/<key>/
#     src/                the checkout, including src/out/Release
#     .gclient            its gclient solution, and .gclient_entries
#     .cipd/              gclient's cipd cache
#     .reproxy_cache/     reclient's deps cache
#     backend/            the checkout RECORD_REPLAY_BACKEND_DIR points at
#     home/              HOME for the build container
#     .replay-checkout    this scheme's stamp: key, scheme, when, from what
#
# <checkouts dir> is $CHROMIUM_CHECKOUTS_DIR, or ~/chromium-checkouts. The
# container mounts <key>/ at /chromium whatever the key is, so every path a
# build sees -- and every path gn, ninja and reclient bake into their state --
# is the same as it was, and as it is for every other key.
#
#   checkout.sh key                 the key for the tree this script came from
#   checkout.sh root [key]          where that key's state lives
#   checkout.sh require [key]       fail unless that key is prepared
#   checkout.sh sync                fetch and reset this key's tree to the build's
#                                   commit, and its DEPS'd Replay forks with it
#   checkout.sh list [--sizes]      the keys that are prepared here
#   checkout.sh adopt <dir> [key]   make an existing checkout a key's (migration)
#   checkout.sh prepare [--from <key>]
#                                   create this key's checkout
#   checkout.sh wipe <key>          remove everything that key has
#   checkout.sh wipe-out <key>      remove only that key's build outputs
#
# Nothing here writes outside the key's own directory, so a build cannot damage
# another version's state, and `wipe` is the whole recovery procedure.
set -euo pipefail

# Bumped when the layout changes in a way that makes older directories
# unusable; `require` then refuses them instead of building in them.
SCHEME=1

dir=$(dirname "$(readlink -f "$0")")
repo=$(dirname "$(dirname "$dir")")

checkouts_dir() {
  echo "${CHROMIUM_CHECKOUTS_DIR:-$HOME/chromium-checkouts}"
}

# The milestone of the tree this script came from. chrome/VERSION is in the tree
# being built, so a job reads it from the small clone it bootstrapped these
# scripts with, before any big checkout is involved: `git show` works there even
# though chrome/ is not checked out.
version_major() {
  local v
  if [ -f "$repo/chrome/VERSION" ]; then
    v=$(sed -n 's/^MAJOR=//p' "$repo/chrome/VERSION")
  else
    v=$(git -C "$repo" show HEAD:chrome/VERSION | sed -n 's/^MAJOR=//p')
  fi
  if [ -z "$v" ]; then
    echo "checkout: no MAJOR in chrome/VERSION of $repo" >&2
    exit 1
  fi
  echo "$v"
}

# The milestone is the key, because that is what decides whether two builds want
# the same tree: a branch rebasing onto a new milestone gets its own checkout
# without anyone arranging it. Two branches on the SAME milestone still share
# one -- for development branches that is the point (they share a warm out dir),
# but a rebase experiment that wants to be alone, or two of them on one
# milestone, set CHROMIUM_CHECKOUT_KEY (per Buildkite build, or on a branch's
# builds) and get a directory nothing else touches.
key() {
  local k=${CHROMIUM_CHECKOUT_KEY:-}
  if [ -z "$k" ]; then
    k=m$(version_major)
  fi
  if ! [[ $k =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
    echo "checkout: '$k' is not a usable key (letters, digits, . _ - only)" >&2
    exit 1
  fi
  echo "$k"
}

root() {
  echo "$(checkouts_dir)/${1:-$(key)}"
}

stamp_file() {
  echo "$(root "${1:-}")/.replay-checkout"
}

write_stamp() {
  local k=$1 how=$2
  cat > "$(stamp_file "$k")" <<EOF
key=$k
scheme=$SCHEME
created=$(date -u +%Y-%m-%dT%H:%M:%SZ)
created_by=$how
EOF
}

require() {
  local k=${1:-$(key)} r
  r=$(root "$k")
  if [ ! -d "$r/src/.git" ]; then
    cat >&2 <<EOF
checkout: no Chromium checkout for key '$k' at $r

A build never creates one and never borrows another version's: prepare it once,
on this host, and every build of that version then has its own tree.

  replay_build_scripts/ci/checkout.sh prepare                 # from scratch
  replay_build_scripts/ci/checkout.sh prepare --from <key>    # from a prepared one
  replay_build_scripts/ci/checkout.sh adopt <dir> $k        # an existing checkout

Prepared keys here: $(ls -1 "$(checkouts_dir)" 2>/dev/null | tr '\n' ' ')
EOF
    exit 1
  fi
  local have
  have=$(sed -n 's/^scheme=//p' "$(stamp_file "$k")" 2>/dev/null || true)
  if [ "$have" != "$SCHEME" ]; then
    echo "checkout: $r is layout scheme '${have:-none}', this tree builds scheme $SCHEME." >&2
    echo "Wipe it and prepare it again: checkout.sh wipe $k" >&2
    exit 1
  fi
  mkdir -p "$r/home"
  echo "$r"
}

# What a job does to this key's tree before building: the fetch and reset that
# used to be pipeline steps against the one shared checkout, plus the Replay
# forks DEPS pins, and the backend checkout the upload step reads. It runs from
# the job's own copy of these scripts (see .buildkite/pipeline.yml), not from
# whatever the tree happens to hold, which is how the cleanup in
# replayio/chromium#1544 came to run a previous job's version of itself.
sync() {
  local k r
  k=$(key)
  r=$(require "$k")
  local commit=${CHROMIUM_REVISION:-${BUILDKITE_COMMIT:-}}
  if [ -z "$commit" ]; then
    echo "checkout: set CHROMIUM_REVISION (or BUILDKITE_COMMIT) to the commit to build" >&2
    exit 1
  fi
  echo "--- checkout '$k' at $r -> $commit"
  # The fork goes to the commit being built, the Replay forks of v8, skia,
  # webrtc and boringssl to their DEPS revisions, and the backend to
  # REPLAY_BACKEND_REV; everything else in the tree is what `prepare` synced.
  # (gclient sync is not run per job: this fork is too old for it, see
  # common.mjs.)
  CHROMIUM_SRC_DIR="$r/src" \
    RECORD_REPLAY_BACKEND_DIR="$r/backend" \
    CHROMIUM_REVISION="$commit" \
    node "$dir/sync-repos.mjs"
}

list() {
  local d
  d=$(checkouts_dir)
  if [ ! -d "$d" ]; then
    echo "no checkouts in $d"
    return
  fi
  local k
  for k in $(ls -1 "$d" 2>/dev/null); do
    if [ ! -e "$d/$k/.replay-checkout" ]; then
      continue
    fi
    local head size=""
    head=$(git -C "$d/$k/src" rev-parse --short=12 HEAD 2>/dev/null || echo "?")
    if [ "${1:-}" = "--sizes" ]; then
      size=" src=$(du -sh "$d/$k/src" 2>/dev/null | cut -f1)"
      size="$size out=$(du -sh "$d/$k/src/out" 2>/dev/null | cut -f1)"
    fi
    echo "$k  HEAD=$head  $(sed -n 's/^created=/created /p' "$d/$k/.replay-checkout")$size"
  done
}

# Migration: the checkout that every build has been sharing becomes the
# milestone's own. Nothing is copied, so this costs nothing and is reversible by
# moving the directory back.
adopt() {
  local src=${1:-} k=${2:-$(key)} r
  if [ -z "$src" ] || [ ! -d "$src/src/.git" ]; then
    echo "usage: $0 adopt <dir holding src/> [key]" >&2
    exit 1
  fi
  r=$(root "$k")
  if [ -e "$r" ]; then
    echo "checkout: $r already exists; wipe it first or pick another key" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$r")"
  mv "$src" "$r"
  mkdir -p "$r/home"
  # Secrets a job wrote into the old shared directory (the EngFlow client cert
  # and key used to land next to the checkout and stay there) do not come along.
  rm -f "$r/engflow.crt" "$r/engflow.key"
  write_stamp "$k" "adopted from $src"
  echo "checkout: '$k' is now $r"
}

prepare() {
  local from="" k r
  while [ $# -gt 0 ]; do
    case $1 in
      --from) from=${2:?--from needs a key}; shift 2 ;;
      *) echo "usage: $0 prepare [--from <key>]" >&2; exit 1 ;;
    esac
  done
  k=$(key)
  r=$(root "$k")
  if [ -e "$r" ]; then
    echo "checkout: $r already exists" >&2
    exit 1
  fi
  mkdir -p "$r"
  if [ -n "$from" ]; then
    local src
    src=$(root "$from")
    if [ ! -d "$src/src/.git" ]; then
      echo "checkout: '$from' is not prepared" >&2
      exit 1
    fi
    echo "--- copying $src -> $r (without build outputs)"
    # Everything but the out dirs: the deps of a neighbouring milestone are a
    # far better starting point for gclient sync than an empty directory, and
    # another version's build outputs are exactly what must not come along.
    rsync -a --delete --exclude '/src/out' --exclude '/home' --exclude '/.replay-checkout' \
      "$src/" "$r/"
  else
    echo "--- cloning replayio/chromium into $r/src"
    git clone git@github.com:replayio/chromium.git "$r/src"
    cat > "$r/.gclient" <<'EOF'
solutions = [
{
   "name": "src",
   "url": "https://chromium.googlesource.com/chromium/src.git",
   "managed": False,
   "custom_deps": {},
   "custom_vars": {},
},
]
EOF
  fi
  if [ ! -d "$r/backend" ]; then
    echo "--- cloning replayio/backend into $r/backend"
    git clone git@github.com:replayio/backend.git "$r/backend"
  fi
  mkdir -p "$r/home"
  write_stamp "$k" "prepare${from:+ --from $from}"
  cat <<EOF

checkout: '$k' is at $r, with the fork checked out but its DEPS not synced.

Sync it in the build image, so depot_tools is the pinned one:

  replay_build_scripts/ci/build-in-container.sh --root $r -- \\
    bash -c 'cd /chromium && gclient sync -D --with_branch_heads && gclient runhooks'

This fork disabled per-build gclient sync ("may be too old for gclient sync to
work properly", common.mjs), so for a milestone nobody has synced here before,
expect this to need attention -- it is the one step of a new version's checkout
that is not routine, and it is also the step a rebase does anyway.
EOF
}

wipe() {
  local k=${1:-} r
  if [ -z "$k" ]; then
    echo "usage: $0 wipe <key>   (an explicit key: this is not undoable)" >&2
    exit 1
  fi
  r=$(root "$k")
  if [ ! -e "$r" ]; then
    echo "checkout: nothing at $r"
    return
  fi
  echo "--- removing $r"
  rm -rf "$r"
}

wipe_out() {
  local k=${1:-$(key)} r
  r=$(root "$k")
  echo "--- removing $r/src/out"
  rm -rf "$r/src/out"
}

case ${1:-} in
  key) key ;;
  root) shift; root "${1:-}" ;;
  require) shift; require "${1:-}" ;;
  sync) sync ;;
  list) shift; list "${1:-}" ;;
  adopt) shift; adopt "$@" ;;
  prepare) shift; prepare "$@" ;;
  wipe) shift; wipe "${1:-}" ;;
  wipe-out) shift; wipe_out "${1:-}" ;;
  *)
    sed -n '/^#   checkout.sh key/,/^# Nothing here writes/p' "$(readlink -f "$0")" |
      sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
