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
# Next to the keys, and belonging to none of them, is .git-cache: gclient's
# mirrors of the repositories DEPS pins, which every version's checkout clones
# its dependencies from. It holds git objects, which are append-only and say
# nothing about a version, so sharing it costs nothing in isolation and saves
# most of what a checkout would otherwise take (see prepare). `wipe` cannot
# touch it -- a key may not begin with a dot.
#
# <checkouts dir> is $CHROMIUM_CHECKOUTS_DIR, or ~/chromium-checkouts. The
# container mounts <key>/ at /chromium whatever the key is, so every path a
# build sees -- and every path gn, ninja and reclient bake into their state --
# is the same as it was, and as it is for every other key.
#
#   checkout.sh key                 the key for the tree this script came from
#   checkout.sh root [key]          where that key's state lives
#   checkout.sh git-cache           where the shared DEPS mirrors live
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

# gclient's mirrors of the DEPS repositories, shared by every version.
git_cache() {
  echo "${GIT_CACHE_PATH:-$(checkouts_dir)/.git-cache}"
}

fork_url() {
  echo "${CHROMIUM_FORK_URL:-git@github.com:replayio/chromium.git}"
}

# The checkout every build shared before this existed, and still shares with
# branches that have not been rebased onto it.
legacy_checkout() {
  echo "${CHROMIUM_LEGACY_CHECKOUT:-$HOME/chromium}"
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

# Any checkout that already has the fork's history, to clone it from. Objects
# are objects whatever version checked them out, and a local clone hardlinks
# them.
nearest_src() {
  local d src
  d=$(checkouts_dir)
  for src in "$d"/*/src; do
    if [ -d "$src/.git" ]; then
      echo "$src"
      return
    fi
  done
  if [ -d "$(legacy_checkout)/src/.git" ]; then
    echo "$(legacy_checkout)/src"
  fi
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

# A key with no directory of its own, whose milestone is the one the shared
# checkout holds, takes that checkout over: a symlink, nothing copied, nothing
# moved, so branches that still build in it keep working. This is how the
# current milestone arrives here without anyone doing anything by hand. A
# milestone nobody has checked out on this machine is a different matter, since
# making one means gclient and the 140 repositories DEPS pins (see prepare).
link_legacy_checkout() {
  local k=$1 r=$2 legacy major
  legacy=$(legacy_checkout)
  if [ ! -d "$legacy/src/.git" ]; then
    return 1
  fi
  major=$(sed -n 's/^MAJOR=//p' "$legacy/src/chrome/VERSION" 2>/dev/null)
  if [ -z "$major" ] || [ "$k" != "m$major" ]; then
    return 1
  fi
  mkdir -p "$(dirname "$r")"
  if ! ln -s "$legacy" "$r" 2>/dev/null; then
    # Another job got there first, which is all this needed.
    [ -d "$r/src/.git" ]
    return
  fi
  mkdir -p "$r/home"
  write_stamp "$k" "linked the shared checkout at $legacy"
  # stderr: require's stdout is the path its callers capture.
  echo "checkout: '$k' -> $legacy, the checkout builds shared before there was" >&2
  echo "one per version. Nothing was copied or moved: branches that still build" >&2
  echo "there keep working, and 'checkout.sh adopt --move' ends that when they" >&2
  echo "are gone." >&2
}

# Prints the key's directory and nothing else: callers capture it.
require() {
  local k=${1:-$(key)} r
  r=$(root "$k")
  if [ ! -d "$r/src/.git" ]; then
    link_legacy_checkout "$k" "$r" || true
  fi
  if [ ! -d "$r/src/.git" ]; then
    cat >&2 <<EOF
checkout: no Chromium checkout for key '$k' at $r

This machine has no tree for that Chromium version, and a build will not make
one or borrow another version's: a Chromium checkout is the fork plus the 140
repositories its DEPS pin, which is gclient's job and an hour of it, not a git
clone. Make it once and every build of that version has it:

  CHROMIUM_REVISION=<commit> replay_build_scripts/ci/checkout.sh prepare --from <key>
  CHROMIUM_REVISION=<commit> replay_build_scripts/ci/checkout.sh prepare
  replay_build_scripts/ci/checkout.sh adopt <dir> $k   # a tree that already exists

Prepared keys here: $(ls -1 "$(checkouts_dir)" 2>/dev/null | tr '\n' ' ')
Shared checkout: $(legacy_checkout), milestone $(sed -n 's/^MAJOR=/m/p' "$(legacy_checkout)/src/chrome/VERSION" 2>/dev/null || echo unknown)
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
  local commit=${CHROMIUM_REVISION:-${BUILDKITE_COMMIT:-}}
  if [ -z "$commit" ]; then
    echo "checkout: set CHROMIUM_REVISION (or BUILDKITE_COMMIT) to the commit to build" >&2
    exit 1
  fi
  # "HEAD" would reset the checkout to whatever it already held and publish
  # that as this build, silently. The step this replaced reset to
  # origin/$BUILDKITE_BRANCH, so it could not happen there.
  if [ "$commit" = "HEAD" ]; then
    echo "checkout: refusing to build 'HEAD': name the commit or branch to build" >&2
    exit 1
  fi
  r=$(require "$k")
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
  local stamp k
  for stamp in "$d"/*/.replay-checkout; do
    if [ ! -e "$stamp" ]; then
      continue
    fi
    k=$(basename "$(dirname "$stamp")")
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
# milestone's own.
#
# By default the key becomes a symlink to it, because .buildkite/pipeline.yml is
# fetched per branch: every branch that has not been rebased onto this change
# still runs the old step, which cd's into /home/ubuntu/chromium/src and builds
# there. A symlink lets both kinds of job build the same tree, so this can land
# without a flag day -- and for the same reason the old build image and
# /home/ubuntu/depot_tools have to stay until those branches are gone.
#
# --move moves the directory instead, for when nothing builds the old way any
# more. Either way it costs nothing and is undone by moving it back.
adopt() {
  local move=0
  while [ $# -gt 0 ]; do
    case ${1:-} in
      --move) move=1; shift ;;
      *) break ;;
    esac
  done
  local src=${1:-} k=${2:-$(key)} r
  if [ -z "$src" ] || [ ! -d "$src/src/.git" ]; then
    echo "usage: $0 adopt [--move] <dir holding src/> [key]" >&2
    exit 1
  fi
  src=$(cd "$src" && pwd)
  r=$(root "$k")
  if [ -e "$r" ] || [ -L "$r" ]; then
    echo "checkout: $r already exists; wipe it first or pick another key" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$r")"
  if [ "$move" = 1 ]; then
    mv "$src" "$r"
  else
    ln -s "$src" "$r"
  fi
  mkdir -p "$r/home"
  # Secrets a job wrote into the old shared directory (the EngFlow client cert
  # and key used to land next to the checkout and stay there) do not come along.
  # A linked checkout keeps them while old-style jobs still need them there.
  if [ "$move" = 1 ]; then
    rm -f "$r/engflow.crt" "$r/engflow.key"
  fi
  write_stamp "$k" "adopted from $src"
  echo "checkout: '$k' is $r -> $(readlink -f "$r")"
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
  if [ -e "$r" ] || [ -L "$r" ]; then
    echo "checkout: $r already exists" >&2
    exit 1
  fi
  local commit=${CHROMIUM_REVISION:-}
  if [ -z "$commit" ]; then
    echo "checkout: set CHROMIUM_REVISION to the commit or branch '$k' is for" >&2
    echo "(the fork's default branch is the current milestone, which is not" >&2
    echo "what a checkout for another one should hold)" >&2
    exit 1
  fi
  # A version costs 60-115 GB here, on the volume every other version and the
  # backend's C++ test jobs share, so say so before taking it.
  local free
  free=$(df -BG --output=avail "$(dirname "$r")" 2>/dev/null | tail -1 | tr -dc 0-9)
  if [ -n "$free" ]; then
    echo "--- $free GB free on $(dirname "$r")"
    if [ "$free" -lt 150 ]; then
      echo "checkout: that is not much for a Chromium checkout and its build;" >&2
      echo "wipe a key that is no longer in flight first (checkout.sh list)." >&2
      echo "Set CHROMIUM_CHECKOUT_FORCE=1 to prepare anyway." >&2
      if [ -z "${CHROMIUM_CHECKOUT_FORCE:-}" ]; then
        exit 1
      fi
    fi
  fi
  mkdir -p "$r"
  # Nothing half-made is left behind for the next attempt to trip over.
  # shellcheck disable=SC2064  # $r is expanded now on purpose: the trap must
  # remove this directory even if the variable is out of scope by then.
  trap "rm -rf '$r'" EXIT
  if [ -n "$from" ]; then
    local src
    src=$(root "$from")
    if [ ! -d "$src/src/.git" ]; then
      echo "checkout: '$from' is not prepared" >&2
      exit 1
    fi
    # Two passes, because the two halves of a checkout can be shared to very
    # different extents. The working files are copied: a gclient hook that
    # overwrites one in place would otherwise write into the version this was
    # copied from. Every .git is hardlinked instead (--link-dest), because git
    # only ever adds pack files and never rewrites one, so two checkouts can
    # share the same inodes safely -- which is what makes this cheap: the
    # fork's object store alone is 58 GB, and hardlinking it costs megabytes.
    echo "--- copying $src -> $r: working files"
    rsync -a --delete --exclude '.git/' \
      --exclude '/src/out' --exclude '/home' --exclude '/.replay-checkout' \
      "$src/" "$r/"
    echo "--- copying $src -> $r: git objects, hardlinked"
    rsync -a --link-dest="$src" --include '*/' --include '.git/***' --exclude '*' \
      "$src/" "$r/"
  else
    # A local clone hardlinks the pack files, so taking the fork's history from
    # a checkout that already has it costs about a second and a couple of
    # megabytes instead of 58 GB and a fetch of the whole repository.
    local near
    near=$(nearest_src)
    if [ -n "$near" ]; then
      echo "--- cloning the fork from $near (hardlinked objects)"
      git clone -q --no-checkout "$near" "$r/src"
      git -C "$r/src" remote set-url origin "$(fork_url)"
    else
      echo "--- cloning the fork from $(fork_url)"
      git clone -q --no-checkout "$(fork_url)" "$r/src"
    fi
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
  echo "--- checking out $commit"
  git -C "$r/src" fetch origin "$commit"
  git -C "$r/src" checkout -f --detach FETCH_HEAD
  if [ ! -d "$r/backend" ]; then
    echo "--- cloning replayio/backend into $r/backend"
    git clone git@github.com:replayio/backend.git "$r/backend"
  fi
  mkdir -p "$r/home"
  write_stamp "$k" "prepare${from:+ --from $from} $commit"
  trap - EXIT
  cat <<EOF

checkout: '$k' is at $r, with the fork checked out but its DEPS not synced.

Sync its DEPS in the build image, so depot_tools is the pinned one and the
generated reclient configs get the paths a build runs at. GIT_CACHE_PATH is
gclient's mirror of every DEPS repository, shared by all versions: with it, the
140 repositories come from disk rather than the network, and each checkout's
copy of them is a clone of the mirror rather than another 14 GB of objects.

  $r/src/replay_build_scripts/ci/build-in-container.sh --root $r -- \\
    bash -c 'cd /chromium && GIT_CACHE_PATH=/chromium-git-cache gclient sync -D --with_branch_heads && gclient runhooks'

(build-in-container.sh mounts $(git_cache) there.)

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
  git-cache) git_cache ;;
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
