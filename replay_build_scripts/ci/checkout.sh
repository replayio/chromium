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
# most of what a checkout would otherwise take (see prepare). A dependency
# borrows the mirror's objects, so every key needs it. `wipe` cannot touch it --
# a key may not begin with a dot.
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
#                                   commit, and its DEPS'd Replay forks with it;
#                                   prepare the key first if it has no checkout
#   checkout.sh backend-rev [key]   the backend commit `sync` checked out
#   checkout.sh list [--sizes]      the keys that are prepared here
#   checkout.sh adopt <dir> [key]   make an existing checkout a key's (migration)
#   checkout.sh prepare [--from <key>]
#                                   create this key's checkout, unless it has one
#   checkout.sh wipe <key>          remove everything that key has
#   checkout.sh wipe-out <key>      remove only that key's build outputs
#   checkout.sh reclaim             remove least recently used state until the
#                                   volume has enough room (see reclaim)
#   checkout.sh use-lock [key]      the lock a job holds, shared, while it uses
#                                   that key (see use)
#   checkout.sh image-used <ref>    record that a job uses that image now
#
# A job writes into its own key's directory, gclient's mirrors in .git-cache,
# which only ever gain objects, and the locks and use records next to the keys.
# Its one write to other state is reclaim: when the volume runs low, the least
# recently used keys and images that no job is using are removed whole. Another
# version then costs its next job a prepare, never a damaged tree, and `wipe` is
# the whole recovery procedure.
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
# being built, so a job reads it from the small clone these scripts came from,
# before any big checkout is involved: the Chromium step checks the file out
# with them, and in a sparse clone without it `git show` still works.
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
  if ! valid_key "$k"; then
    echo "checkout: '$k' is not a usable key (letters, digits, . _ - only)" >&2
    exit 1
  fi
  echo "$k"
}

# Never beginning with a dot, so nothing a key owns can be named like the shared
# state next to the keys.
valid_key() {
  [[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
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

# Prints the key's directory and nothing else: callers capture it.
require() {
  local k=${1:-$(key)} r
  r=$(root "$k")
  # A key is that literal directory. A symlink is how the retired shared
  # checkout was claimed by whichever milestone built last (#1637, build 5567):
  # refuse one rather than build in a tree that belongs to another version.
  if [ -L "$r" ]; then
    echo "checkout: $r is a symlink to $(readlink "$r"), not a checkout of its own." >&2
    echo "Remove the link and prepare '$k': checkout.sh prepare" >&2
    exit 1
  fi
  if [ ! -d "$r/src/.git" ]; then
    echo "checkout: no Chromium checkout for key '$k' at $r: checkout.sh sync or prepare makes it" >&2
    exit 1
  fi
  local have
  have=$(sed -n 's/^scheme=//p' "$(stamp_file "$k")" 2>/dev/null || true)
  if [ "$have" != "$SCHEME" ]; then
    echo "checkout: $r is layout scheme '${have:-none}', this tree builds scheme $SCHEME." >&2
    echo "Wipe it and prepare it again: checkout.sh wipe $k" >&2
    exit 1
  fi
  # The stamp's mtime is when a job last used the key: reclaim's recency.
  touch -c "$(stamp_file "$k")"
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
  use "$k"
  ensure_prepared "$k" "$commit"
  reclaim "$k"
  r=$(require "$k")
  echo "--- checkout '$k' at $r -> $commit"
  # The fork goes to the commit being built, the Replay forks of v8, skia,
  # webrtc and boringssl to their DEPS revisions, and the backend to
  # REPLAY_BACKEND_REV. The backend pin is the commit's file, never env: a
  # build's inputs must not depend on how it was started.
  env -u REPLAY_BACKEND_REV \
    CHROMIUM_SRC_DIR="$r/src" \
    RECORD_REPLAY_BACKEND_DIR="$r/backend" \
    CHROMIUM_REVISION="$commit" \
    "$dir/in-tools-image.sh" "$r" node "$dir/sync-repos.mjs"
  # Then everything else DEPS names, at this commit's revisions. src is already
  # there, so gclient reads the DEPS of the commit being built.
  #
  # Per build, not once when the key was prepared. A key outlives the commits
  # built in it, so a commit that moves a pin other than the four forks, adds or
  # drops a dependency, or changes a hook has to take effect. Until now none of
  # that did: the checkout kept whatever the first build on that host resolved,
  # two hosts could build one commit against different dependencies, and the
  # pool's daily image spread one host's state to new ones.
  #
  # The reason this was once skipped does not hold here. #1107 found that
  # `gclient sync -D` will not move a dependency back to an older revision "if
  # the sha exists on the currently checked out branch" -- which is a dependency
  # whose HEAD is a named branch, as the hand-managed tree of the time had.
  # gclient leaves every checkout it creates detached, and for a detached HEAD
  # it checks the revision out in either direction (depot_tools gclient_scm.py,
  # case 0 of GitWrapper.update). src is the one thing it would not move, and it
  # is unmanaged here and synced above.
  echo "--- gclient sync and hooks"
  "$dir/build-in-container.sh" --root "$r" -- \
    bash -c 'cd /chromium && gclient sync -D --with_branch_heads && gclient runhooks'
}

backend_rev() {
  git -C "$(require "${1:-}")/backend" rev-parse HEAD
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
    echo "$k  HEAD=$head  $(sed -n 's/^created=/created /p' "$stamp")  used $(date -u -d "@$(last_used "$k")" +%Y-%m-%dT%H:%M:%SZ)$size"
  done
}

# Migration: the checkout that every build has been sharing becomes the
# milestone's own.
#
# By default the key becomes a symlink to it, so that the step from before this
# change, which cd's into /home/ubuntu/chromium/src and builds there, and the
# new one build the same tree. chromium-build now runs each commit's own
# pipeline.yml and refuses commits without ci/ (its pipeline settings), so only
# other ways of running the old step still need the link, the old build image
# and /home/ubuntu/depot_tools.
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

# The stamp is written last, so a key without one has no checkout a build can
# use: nothing is there, or a prepare that did not finish left it.
unprepared() {
  [ ! -L "$(root "$1")" ] && [ ! -e "$(stamp_file "$1")" ]
}

# One prepare of a key at a time on this machine: a second job of that version
# waits for the first one's, then builds in it. The lock is next to the keys,
# named as no key can be (a key may not begin with a dot).
ensure_prepared() {
  local k=$1 lock
  unprepared "$k" || return 0
  mkdir -p "$(checkouts_dir)"
  exec {lock}>"$(checkouts_dir)/.$k.lock"
  if ! flock -n "$lock"; then
    echo "--- waiting for another job's prepare of '$k'"
    flock "$lock"
  fi
  if unprepared "$k"; then
    prepare_key "$@"
  fi
  exec {lock}>&-
}

# A job holds its key's use lock, shared, for as long as it uses the key: the
# pipeline step for the whole job, and `sync`, `prepare` and
# build-in-container.sh for their own run. Removing a key's state takes it
# exclusively and gives up if anyone holds it, so nothing removes a tree that a
# job is using.
use_lock() {
  mkdir -p "$(checkouts_dir)"
  echo "$(checkouts_dir)/.$1.use"
}

# use <key>: hold <key>'s use lock until this process exits.
use() {
  local fd
  exec {fd}>"$(use_lock "$1")"
  if ! flock -n -s "$fd"; then
    echo "--- waiting for another job's removal of '$1'"
    flock -s "$fd"
  fi
}

# unused <key> <command...>: run the command holding <key>'s use lock
# exclusively, or fail without running it if a job is using the key.
unused() {
  local k=$1 fd status=0
  shift
  exec {fd}>"$(use_lock "$k")"
  if ! flock -n -x "$fd"; then
    echo "checkout: a job is using '$k'; leaving it" >&2
    exec {fd}>&-
    return 1
  fi
  "$@" || status=$?
  exec {fd}>&-
  return "$status"
}

# Out of the way first, in one rename: a removal cut short leaves no half-removed
# key for the next job of any scheme to trip over, only trash, which reclaim
# removes first.
remove_key() {
  local r trash
  r=$(root "$1")
  echo "--- removing $r"
  if [ -L "$r" ]; then
    rm -f "$r"
    return
  fi
  trash=$(mktemp -d "$(checkouts_dir)/.removing.XXXXXX")
  mv "$r" "$trash/" && rm -rf "$trash"
}

# When a job last used a key, in epoch seconds: its stamp's mtime (require
# touches it), or the newest file at the top of its out dir, which the build of
# any scheme writes (its logs). 0 if there is neither.
last_used() {
  local r t
  r=$(root "$1")
  t=$({ find "$(stamp_file "$1")" "$r/src/out/Release" -maxdepth 1 -printf '%T@\n' 2>/dev/null || true; } |
    sort -n | tail -1)
  t=${t%.*}
  echo "${t:-0}"
}

# The space a host keeps free, in GB: one more version's checkout and its build.
min_free_gb() {
  echo "${CHROMIUM_CHECKOUTS_MIN_FREE_GB:-150}"
}

free_gb() {
  df -BG --output=avail "$(checkouts_dir)" | tail -1 | tr -dc 0-9
}

# When a job last used an image (build-image.sh's `ensure`).
image_mark() {
  echo "$(checkouts_dir)/.images/$1"
}

image_used() {
  local mark
  mark=$(image_mark "${1:?usage: $0 image-used <image ref>}")
  mkdir -p "$(dirname "$mark")"
  touch "$mark"
}

# lru [<key>]: the state reclaim may remove, least recently used first, one
# "<last used, epoch seconds> key|image|trash <key, image ref or path>" per
# line. Neither <key> nor this tree's images are listed: this job is about to
# use them. Trash, and a key or image with no record of use, sort first. Images
# are this script line's (build-image.sh); other lines' are theirs to manage.
lru() {
  local own=${1:-} r k
  for r in "$(checkouts_dir)"/.removing.*; do
    if [ -e "$r" ]; then
      echo "0 trash $r"
    fi
  done
  for r in "$(checkouts_dir)"/*/; do
    r=${r%/}
    k=${r##*/}
    # A symlinked key owns nothing to free (adopt).
    if [ -L "$r" ] || ! valid_key "$k" || [ "$k" = "$own" ]; then
      continue
    fi
    echo "$(last_used "$k") key $k"
  done
  local keep=("$("$dir/build-image.sh" ref)" "$("$dir/build-image.sh" --tools ref)")
  local repo ref
  for repo in "${keep[@]%:*}"; do
    while read -r ref; do
      if [ "$ref" != "${keep[0]}" ] && [ "$ref" != "${keep[1]}" ]; then
        echo "$(stat -c %Y "$(image_mark "$ref")" 2>/dev/null || echo 0) image $ref"
      fi
    done < <(docker image ls --format '{{.Repository}}:{{.Tag}}' "$repo")
  done
}

# reclaim [<key>]: while the volume the keys share has less than min_free_gb
# free, remove the least recently used key or image that no job is using. A
# key's use is last_used, an image's its mark (image-used).
# One reclaim at a time on this machine, so that two jobs short of the same
# space do not both remove state for it.
reclaim() {
  local own=${1:-} min free lock kind id
  mkdir -p "$(checkouts_dir)"
  min=$(min_free_gb)
  free=$(free_gb)
  if [ "$free" -ge "$min" ]; then
    return 0
  fi
  exec {lock}<"$(checkouts_dir)"
  flock "$lock"
  while read -r _ kind id; do
    free=$(free_gb)
    if [ "$free" -ge "$min" ]; then
      break
    fi
    echo "--- $free GB free on $(checkouts_dir), below $min: removing $kind '$id', least recently used first"
    case $kind in
      trash) rm -rf "$id" || true ;;
      key) unused "$id" remove_key "$id" || true ;;
      # docker refuses an image that a container uses.
      image) docker image rm "$id" && rm -f "$(image_mark "$id")" || true ;;
    esac
  done < <(lru "$own" | sort -n)
  exec {lock}<&-
}

# prepare_key <key> <commit> [<from key>]: the fork at the commit, everything
# its DEPS pin, the backend, and last the stamp. Writes into the key's
# directory only, apart from gclient's mirrors.
prepare_key() {
  local k=$1 commit=$2 from=${3:-} r
  r=$(root "$k")
  if [ -e "$r" ]; then
    echo "--- removing $r: a prepare that did not finish left it"
    rm -rf "$r"
  fi
  # A version takes tens of GB here, on the volume every other version shares.
  reclaim "$k"
  local free
  free=$(free_gb)
  echo "--- $free GB free on $(checkouts_dir)"
  if [ "$free" -lt "$(min_free_gb)" ]; then
    echo "checkout: that is not enough for a Chromium checkout and its build," >&2
    echo "and reclaim found nothing more that no job is using (checkout.sh list)." >&2
    echo "Set CHROMIUM_CHECKOUT_FORCE=1 to prepare anyway." >&2
    if [ -z "${CHROMIUM_CHECKOUT_FORCE:-}" ]; then
      exit 1
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
    # share the same inodes safely -- which is what makes this cheap: a fork
    # with its history is 58 GB, and hardlinking it costs megabytes.
    echo "--- copying $src -> $r: working files"
    rsync -a --delete --exclude '.git/' \
      --exclude '/src/out' --exclude '/home' --exclude '/.replay-checkout' \
      "$src/" "$r/"
    echo "--- copying $src -> $r: git objects, hardlinked"
    rsync -a --link-dest="$src" --include '*/' --include '.git/***' --exclude '*' \
      "$src/" "$r/"
  else
    git init -q "$r/src"
    git -C "$r/src" remote add origin "$(fork_url)"
    write_gclient "$r/.gclient"
  fi
  # One commit, no history, unless the copy has it: the fork's history is 58 GB
  # and a build reads none of it.
  local depth=(--depth 1)
  if [ -n "$from" ] && [ "$(git -C "$r/src" rev-parse --is-shallow-repository)" = false ]; then
    depth=()
  fi
  echo "--- fetching $commit from $(fork_url)"
  git -C "$r/src" fetch "${depth[@]}" origin "$commit"
  git -C "$r/src" checkout -q -f --detach FETCH_HEAD
  if [ ! -d "$r/backend" ]; then
    echo "--- cloning replayio/backend into $r/backend"
    git clone git@github.com:replayio/backend.git "$r/backend"
  fi
  # In the build image: depot_tools is the pinned one, and the generated
  # configs get the paths a build runs at. GIT_CACHE_PATH (build-in-container.sh)
  # is gclient's mirror of every DEPS repository, shared by all versions: once
  # any version has a repository, a checkout clones it from disk, borrowing the
  # mirror's objects.
  echo "--- gclient sync and hooks"
  "$dir/build-in-container.sh" --root "$r" -- \
    bash -c 'cd /chromium && gclient sync -D --with_branch_heads && gclient runhooks'
  mkdir -p "$r/home"
  write_stamp "$k" "prepare${from:+ --from $from} $commit"
  trap - EXIT
}

# The gclient solution of a checkout prepared from nothing.
# - checkout_traffic_annotation_tools gates a hook that downloads a prebuilt
#   auditor for the network-annotations presubmit. The object m108 pins is gone
#   from gs://chromium-tools-traffic_annotation -- it 404s, while the other
#   buckets the same hook reads answer 200 -- and gclient stops at the first
#   hook that fails, so everything after it is skipped. Nothing in the chrome
#   target wants that binary. Upstream dropped the hook after m108, and gclient
#   ignores a custom_var the DEPS no longer declare (merge_vars in gclient.py),
#   so this stays correct as the fork moves on.
# - lastchange.py stamps the newest commit with a Change-Id trailer, which a
#   one-commit fetch of a Replay commit does not have, and m108's then fails on
#   the empty `git log`. --filter= stamps the commit being built, as the 157
#   checkouts do. A custom hook replaces the DEPS hook of its name.
write_gclient() {
  cat > "$1" <<'EOF'
solutions = [
{
   "name": "src",
   "url": "https://chromium.googlesource.com/chromium/src.git",
   "managed": False,
   "custom_deps": {},
   "custom_vars": {"checkout_traffic_annotation_tools": False},
   "custom_hooks": [
     {
       "name": "lastchange",
       "pattern": ".",
       "action": ["python3", "src/build/util/lastchange.py", "--filter=",
                  "-o", "src/build/util/LASTCHANGE"],
     },
     {
       "name": "gpu_lists_version",
       "pattern": ".",
       "action": ["python3", "src/build/util/lastchange.py", "--filter=",
                  "-m", "GPU_LISTS_VERSION", "--revision-id-only",
                  "--header", "src/gpu/config/gpu_lists_version.h"],
     },
   ],
},
]
EOF
}

prepare() {
  local from=""
  while [ $# -gt 0 ]; do
    case $1 in
      --from) from=${2:?--from needs a key}; shift 2 ;;
      *) echo "usage: $0 prepare [--from <key>]" >&2; exit 1 ;;
    esac
  done
  local commit=${CHROMIUM_REVISION:-}
  if [ -z "$commit" ]; then
    echo "checkout: set CHROMIUM_REVISION to the commit or branch '$(key)' is for" >&2
    echo "(the fork's default branch is the current milestone, which is not" >&2
    echo "what a checkout for another one should hold)" >&2
    exit 1
  fi
  local k
  k=$(key)
  use "$k"
  ensure_prepared "$k" "$commit" "$from"
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
  unused "$k" remove_key "$k"
}

wipe_out() {
  local k=${1:-$(key)} r
  r=$(root "$k")
  echo "--- removing $r/src/out"
  unused "$k" rm -rf "$r/src/out"
}

case ${1:-} in
  key) key ;;
  git-cache) git_cache ;;
  root) shift; root "${1:-}" ;;
  require) shift; require "${1:-}" ;;
  sync) sync ;;
  backend-rev) shift; backend_rev "${1:-}" ;;
  list) shift; list "${1:-}" ;;
  adopt) shift; adopt "$@" ;;
  prepare) shift; prepare "$@" ;;
  wipe) shift; wipe "${1:-}" ;;
  wipe-out) shift; wipe_out "${1:-}" ;;
  reclaim) reclaim ;;
  use-lock) shift; use_lock "${1:-$(key)}" ;;
  image-used) shift; image_used "${1:-}" ;;
  *)
    sed -n '/^#   checkout.sh key/,/^# A job writes/p' "$(readlink -f "$0")" |
      sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
