#!/bin/bash
# One Chromium checkout per Chromium version, each with all of its own state.
#
# Every build on the agent used to run in /home/ubuntu/chromium: one checkout,
# one out dir, one gclient solution, one backend checkout, one reclient cache,
# shared by every branch that built there. Two Chromium versions in flight would
# rebuild each other's trees on every job, and a job that damaged that state
# (replayio/chromium#1540, #1544) took every later build with it, with nothing
# recording what had changed.
#
# That directory is gone, and with it the bridge this script used to keep to it.
# A key whose milestone matched whatever the shared tree currently held took it
# over by symlink, which read the identity of a 104 GB development checkout off
# a value any build could change: the pipeline-upload steps reset that tree to
# `origin/$BUILDKITE_BRANCH` on every build of every branch, so a push to a
# rebase branch left it on another milestone and the next build of THAT
# milestone claimed the development checkout as its own (build 5567 took the
# m108 tree as m110 and died in `gn gen`, because only the four Replay forks
# move per job and the other 136 DEPS repositories stayed at m108's pins).
# A key is now a directory somebody prepared, or it does not exist.
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
# them. With no key prepared at all there is nothing to hardlink from and
# `prepare` falls back to cloning the fork over the network: 58 GB and a long
# wait, rather than a second and a couple of megabytes. That is the price of
# wiping the last key on the machine, so keep one.
nearest_src() {
  local d src
  d=$(checkouts_dir)
  for src in "$d"/*/src; do
    if [ -d "$src/.git" ]; then
      echo "$src"
      return
    fi
  done
}

stamp_file() {
  echo "$(root "${1:-}")/.replay-checkout"
}

# What the DEPS of a synced tree pin, apart from the four Replay forks that a
# job moves itself. Everything else in the tree -- pdfium, the toolchain, the
# other 136 repositories -- is whatever the last `gclient sync` of this key
# left, so when this changes, the tree on disk no longer matches the commit
# being built and the build fails somewhere far from the cause.
deps_fingerprint() {
  sed -e "/'v8_revision':/d" -e "/'skia_revision':/d" \
      -e "/'boringssl_revision':/d" -e "/replayio\/chromium-webrtc/d" |
    sha1sum | cut -c1-12
}

write_stamp() {
  local k=$1 how=$2 deps=${3:-}
  cat > "$(stamp_file "$k")" <<EOF
key=$k
scheme=$SCHEME
created=$(date -u +%Y-%m-%dT%H:%M:%SZ)
created_by=$how
deps=$deps
EOF
}

# git only ever adds pack files, which is what lets `prepare --from` hardlink a
# 58 GB object store for a couple of megabytes -- but `gc` rewrites packs, and
# every job runs a full `git fetch origin` in this key's fork and in each of the
# four Replay forks, so `gc --auto` fires on its own sooner or later. The first
# key to repack stops sharing inodes with the others and the volume grows by
# another object store, silently. Repacking a key is a deliberate act here, not
# something a build does on the way past.
disable_auto_gc() {
  local r=$1 d
  for d in "$r/src" "$r/src/v8" "$r/src/third_party/skia" \
           "$r/src/third_party/webrtc" "$r/src/third_party/boringssl/src" \
           "$r/backend"; do
    if [ -d "$d/.git" ] || [ -f "$d/.git" ]; then
      git -C "$d" config gc.auto 0 || true
    fi
  done
}

# Prints the key's directory and nothing else: callers capture it.
require() {
  local k=${1:-$(key)} r
  r=$(root "$k")
  if [ ! -d "$r/src/.git" ]; then
    local message
    message=$(cat <<EOF
checkout: no Chromium checkout for key '$k' at $r

This machine has no tree for that Chromium version, and a build will not make
one or borrow another version's: a Chromium checkout is the fork plus the 140
repositories its DEPS pin, which is gclient's job and an hour of it, not a git
clone. Make it once and every build of that version has it:

  CHROMIUM_REVISION=<commit> replay_build_scripts/ci/checkout.sh prepare --from <key>
  CHROMIUM_REVISION=<commit> replay_build_scripts/ci/checkout.sh prepare
  replay_build_scripts/ci/checkout.sh adopt <dir> $k   # a tree that already exists

Prepared keys here: $(ls -1 "$(checkouts_dir)" 2>/dev/null | tr '\n' ' ')
EOF
)
    echo "$message" >&2
    # The same text on the build, so an unprovisioned milestone says what it
    # needs without anyone opening the log.
    if command -v buildkite-agent >/dev/null 2>&1; then
      printf '%s\n' "**No Chromium checkout for \`$k\` on this agent.**" '' '```' "$message" '```' |
        buildkite-agent annotate --context "checkout-$k" --style error >/dev/null 2>&1 || true
    fi
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
  disable_auto_gc "$r"

  # What this build is about to cost and churn, before it costs it. Every key
  # shares one volume with every other and with the backend's C++ test jobs, so
  # `df` is the number that decides whether the next milestone fits; the out dir
  # is the part that grows per build; and the file count is what a warm out dir
  # is worth, which is the whole reason two branches on one milestone share a
  # checkout. Full per-key sizes are `checkout.sh list --sizes`, not this.
  df -h "$r" 2>/dev/null | tail -1 | sed 's/^/    volume: /' || true
  echo "    out dir: $(du -sh "$r/src/out" 2>/dev/null | cut -f1 || echo none)"
  if git -C "$r/src" fetch -q origin "$commit" 2>/dev/null; then
    echo "    tree: $(git -C "$r/src" rev-parse --short=12 HEAD 2>/dev/null || echo none) -> ${commit:0:12}, $(git -C "$r/src" diff --name-only HEAD FETCH_HEAD 2>/dev/null | wc -l) files differ"
    # Only the four Replay forks move per job, so if the commit being built
    # pins anything else differently from what was synced into this key, the
    # tree on disk is a mix of two milestones: src from this commit, pdfium and
    # the other 135 repositories from the last `gclient sync`. That is what
    # build 5567 was, and it surfaced as an undefined gn identifier deep in
    # pdfium.gni instead of as the version skew it is.
    local want have
    want=$(git -C "$r/src" show FETCH_HEAD:DEPS 2>/dev/null | deps_fingerprint || true)
    have=$(sed -n 's/^deps=//p' "$(stamp_file "$k")" 2>/dev/null || true)
    if [ -n "$want" ] && [ -n "$have" ] && [ "$want" != "$have" ]; then
      echo "    DEPS: $have (synced) != $want (this commit)" >&2
      echo "checkout: '$k' was synced for different DEPS than this commit pins." >&2
      echo "Everything but v8, skia, webrtc and boringssl is still at the" >&2
      echo "revisions gclient last synced here, so a build may fail in a" >&2
      echo "dependency for no reason visible in this commit. Re-sync the key:" >&2
      echo "  checkout.sh prepare   # prints the gclient sync to run in the image" >&2
    fi
  fi

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
  local stamp k keys=()
  for stamp in "$d"/*/.replay-checkout; do
    if [ ! -e "$stamp" ]; then
      continue
    fi
    keys+=("$(basename "$(dirname "$stamp")")")
  done
  if [ ${#keys[@]} -eq 0 ]; then
    echo "no checkouts in $d"
    return
  fi

  # One du over every key at once, never one per key. du counts a hardlinked
  # file for the first name it sees and skips the rest -- but only within a
  # single invocation, so a du per key charges each one the whole 58 GB object
  # store that `prepare --from` hardlinked, and the keys appear to cost 104 GB
  # each when together they cost that once plus their working files. Getting
  # this wrong is how 40 milestones look like 4 TB instead of a few hundred GB.
  local sizes=""
  if [ "${1:-}" = "--sizes" ]; then
    sizes=$(cd "$d" && du -sh "${keys[@]}" 2>/dev/null) || sizes=""
    echo "--- sizes: blocks shared by hardlink are charged to the first key"
    echo "    below that holds them, so a later key's disk= is what it adds."
  fi

  for k in "${keys[@]}"; do
    local head extra=""
    head=$(git -C "$d/$k/src" rev-parse --short=12 HEAD 2>/dev/null || echo "?")
    if [ "${1:-}" = "--sizes" ]; then
      extra=" disk=$(printf '%s\n' "$sizes" | awk -v k="$k" '$2==k{print $1}')"
      extra="$extra out=$(du -sh "$d/$k/src/out" 2>/dev/null | cut -f1)"
      # Packs, because sharing decays: `prepare --from` hardlinks the packs it
      # found, and every later fetch adds packs of this key's own. A key with
      # many more packs than the others has stopped being nearly free.
      extra="$extra packs=$(find "$d/$k/src/.git/objects/pack" -name '*.pack' 2>/dev/null | wc -l)"
    fi
    echo "$k  HEAD=$head  $(sed -n 's/^created=/created /p' "$d/$k/.replay-checkout")$extra"
  done
  if [ "${1:-}" = "--sizes" ]; then
    echo "--- totals (hardlinks counted once across all keys)"
    (cd "$d" && du -shc "${keys[@]}" 2>/dev/null | tail -1) || true
    df -h "$d" | tail -1
  fi
}

# A checkout that already exists somewhere else becomes a key's: the cheap way
# to give a milestone a tree without an hour of gclient, and how the directory
# every build used to share became m108's.
#
# --move moves the directory, which is what ending that sharing meant; without
# it the key is a symlink to a tree somebody else may also be writing to, which
# is only right while something still builds there. Pass the key explicitly
# when the tree is not sitting on the milestone it belongs to -- the default
# key comes from this script's own tree, not from the directory being adopted.
#
# Either way it costs nothing and is undone by moving it back.
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
  # Whatever this tree's DEPS pin now is what its dependencies are synced for,
  # so record it: `sync` compares it with the commit being built and says so
  # when they differ, instead of letting the build fail inside pdfium.
  write_stamp "$k" "adopted from $src" "$(deps_fingerprint < "$r/src/DEPS" 2>/dev/null || true)"
  disable_auto_gc "$r"
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
  # The fingerprint of the DEPS the gclient sync below is about to be run for,
  # so a later build of a commit that pins anything else is told rather than
  # left to fail inside whichever dependency moved.
  write_stamp "$k" "prepare${from:+ --from $from} $commit" \
    "$(deps_fingerprint < "$r/src/DEPS" 2>/dev/null || true)"
  disable_auto_gc "$r"
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
