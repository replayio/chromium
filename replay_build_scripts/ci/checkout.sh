#!/bin/bash
# One Chromium checkout per Chromium version on a build agent: one directory,
# CHECKOUT_ROOT (config.sh), holding everything a build of that version keeps
# there. The version is the key, read from chrome/VERSION (config.sh).
#
#   <root>/src               the tree at the commit being built, with src/out
#   <root>/.gclient          the gclient solution (./gclient), .gclient_entries, .cipd
#   <root>/home              HOME inside the build container
#   <root>/jobs/<job id>     the current job's logs and artifacts (job.sh)
#   <root>/.replay-checkout  stamp: key, scheme, when
#
#   checkout.sh root          print <root>
#   checkout.sh sync          create <root> if it is missing, put src at this
#                             commit, gclient sync its DEPS and run the hooks
#   checkout.sh wipe-out      remove the build outputs (src/out)
#   checkout.sh wipe          remove <root>
#   checkout.sh reclaim       remove least recently used state that no job is
#                             using, until the volume has room again
#   checkout.sh use-lock      the lock a job holds, shared, while it uses <root>
#   checkout.sh image-used <ref>
#                             record that a job uses that image now
#
# "This commit" is HEAD of the repository these scripts are in: in a job, the
# clone the pipeline step made of the commit being built.
#
# Adapted from master's and chromium-158-buildkite's checkout.sh. Removed on
# purpose: keys derived from chrome/VERSION or the environment, linking or
# adopting an existing checkout, cloning from another checkout, the shared
# .git-cache. `sync` converges from any state, so nothing is deleted on failure.
#
# Every key on the host shares one volume, and every script line that builds
# here -- 108's, Electron's -- reclaims it the same way: when it runs low, a job
# removes the least recently used key or image that no job is using, this key
# included if another line's job is the one that is short. A job holds its key's
# use lock while it builds and a removal needs that lock exclusively, so a
# checkout a job is using is never taken from under it. Of the images, a line
# removes only its own (IMAGE_REPO).
set -euo pipefail

dir=$(dirname "$(readlink -f "$0")")
repo=$(dirname "$(dirname "$dir")")
# shellcheck source=config.sh
. "$dir/config.sh"

write_stamp() {
  cat > "$CHECKOUT_STAMP" <<EOF
key=$CHECKOUT_KEY
scheme=$CHECKOUT_SCHEME
created=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
}

# A job holds its key's use lock, shared, for as long as it uses the key: job.sh
# for the whole job, and `sync` for its own run. Removing a key's state takes it
# exclusively and gives up if anyone holds it, so nothing removes a tree that a
# job is using. The lock is beside the keys rather than inside one, so it
# outlives the directory and every script line takes the same one for a key.
use_lock() {
  mkdir -p "$CHECKOUTS_DIR"
  echo "$CHECKOUTS_DIR/.$1.use"
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
  local r=$CHECKOUTS_DIR/$1 trash
  echo "--- removing $r"
  if [ -L "$r" ]; then
    rm -f "$r"
    return
  fi
  trash=$(mktemp -d "$CHECKOUTS_DIR/.removing.XXXXXX")
  mv "$r" "$trash/" && rm -rf "$trash"
}

# When a job last used a key, in epoch seconds: its stamp's mtime (ensure_root
# touches it), or the newest file at the top of its out dir, which the build of
# any scheme writes (its logs). 0 if there is neither.
last_used() {
  local r=$CHECKOUTS_DIR/$1 t
  t=$({ find "$r/.replay-checkout" "$r/src/out/Release" -maxdepth 1 -printf '%T@\n' 2>/dev/null || true; } |
    sort -n | tail -1)
  t=${t%.*}
  echo "${t:-0}"
}

free_gb() {
  df -BG --output=avail "$CHECKOUTS_DIR" | tail -1 | tr -dc 0-9
}

# When a job last used an image (build-image.sh's `ensure`).
image_mark() {
  echo "$CHECKOUTS_DIR/.images/$1"
}

image_used() {
  local mark
  mark=$(image_mark "${1:?usage: $0 image-used <image ref>}")
  mkdir -p "$(dirname "$mark")"
  touch "$mark"
}

# The state reclaim may remove, least recently used first, one
# "<last used, epoch seconds> key|image|trash <key, image ref or path>" per
# line. Neither this key nor this tree's image is listed: this job is about to
# use them. Trash, and a key or image with no record of use, sort first. Images
# are this script line's (build-image.sh); other lines' are theirs to manage.
lru() {
  local r k keep
  for r in "$CHECKOUTS_DIR"/.removing.*; do
    if [ -e "$r" ]; then
      echo "0 trash $r"
    fi
  done
  for r in "$CHECKOUTS_DIR"/*/; do
    r=${r%/}
    k=${r##*/}
    # A symlinked key owns nothing to free.
    if [ -L "$r" ] || ! valid_key "$k" || [ "$k" = "$CHECKOUT_KEY" ]; then
      continue
    fi
    echo "$(last_used "$k") key $k"
  done
  keep=$("$dir/build-image.sh" ref)
  while read -r r; do
    if [ "$r" != "$keep" ]; then
      echo "$(stat -c %Y "$(image_mark "$r")" 2>/dev/null || echo 0) image $r"
    fi
  done < <(docker image ls --format '{{.Repository}}:{{.Tag}}' "$IMAGE_REPO")
}

# reclaim [<GB>]: while the volume the keys share has less than that much free,
# remove the least recently used key or image that no job is using. A key's use
# is last_used, an image's its mark (image-used).
# One reclaim at a time on this machine, so that two jobs short of the same
# space do not both remove state for it.
reclaim() {
  local min=${1:-$PREPARE_MIN_FREE_GB} free lock kind id
  mkdir -p "$CHECKOUTS_DIR"
  free=$(free_gb)
  if [ "$free" -ge "$min" ]; then
    return 0
  fi
  exec {lock}<"$CHECKOUTS_DIR"
  flock "$lock"
  while read -r _ kind id; do
    free=$(free_gb)
    if [ "$free" -ge "$min" ]; then
      break
    fi
    echo "--- $free GB free on $CHECKOUTS_DIR, below $min: removing $kind '$id', least recently used first"
    case $kind in
      trash) rm -rf "$id" || true ;;
      key) unused "$id" remove_key "$id" || true ;;
      # docker refuses an image that a container uses.
      image) docker image rm "$id" && rm -f "$(image_mark "$id")" || true ;;
    esac
  done < <(lru | sort -n)
  exec {lock}<&-
  echo "--- $(free_gb) GB free on $CHECKOUTS_DIR after reclaim (minimum $min)"
}

ensure_root() {
  local min
  assert_isolated_root
  mkdir -p "$CHECKOUTS_DIR"
  # Making a checkout needs room for the whole of one; a key that already has
  # one needs room for its build.
  if [ ! -d "$CHECKOUT_ROOT/src/.git" ]; then
    min=$PREPARE_MIN_FREE_GB
  else
    min=$BUILD_MIN_FREE_GB
  fi
  reclaim "$min"
  require_free_gb "$CHECKOUTS_DIR" "$min"
  if [ ! -e "$CHECKOUT_ROOT" ]; then
    echo "--- creating $CHECKOUT_ROOT"
    mkdir "$CHECKOUT_ROOT"
    write_stamp
  fi
  if [ "$(stamp_value scheme)" != "$CHECKOUT_SCHEME" ]; then
    die "$CHECKOUT_ROOT is layout scheme '$(stamp_value scheme)', this tree builds scheme $CHECKOUT_SCHEME: checkout.sh wipe"
  fi
  mkdir -p "$CHECKOUT_ROOT/src" "$CHECKOUT_ROOT/home"
  # The stamp's mtime is what reclaim judges this key's last use by, so that a
  # version which builds rarely is not taken for abandoned state.
  touch "$CHECKOUT_STAMP"
}

# A fetch that was killed (canceled job, timeout) leaves its partial pack as
# tmp_pack_* in the repository's objects/pack, and nothing removes it: the
# checkouts never gc, and a full fetch of the fork is ~56 GB. Every repository
# of the checkout is covered. Only files from before this run that no process
# has open are removed: the job holds the checkout's concurrency group, so
# such a file belongs to no running fetch.
remove_stale_tmp_packs() {
  local started pack file holder
  [ -d "$CHECKOUT_ROOT/src" ] || return 0
  started=$(mktemp)
  while IFS= read -r -d '' pack; do
    while IFS= read -r -d '' file; do
      holder=$(find /proc/[0-9]*/fd -maxdepth 1 -lname "$file" -print -quit 2>/dev/null || true)
      if [ -n "$holder" ]; then
        echo "keeping $file: open ($holder)"
        continue
      fi
      echo "removing stale $(du -h "$file" | cut -f1) $file"
      rm -f "$file"
    done < <(find "$pack" -maxdepth 1 -type f -name 'tmp_pack_*' ! -newer "$started" -print0)
  done < <(find "$CHECKOUT_ROOT/src" -path "$CHECKOUT_ROOT/src/out" -prune -o \
    -type d -path '*/.git/objects/pack' -print0)
  rm -f "$started"
}

sync() {
  local commit url src=$CHECKOUT_ROOT/src lock holder
  commit=$(git -C "$repo" rev-parse HEAD)
  url=$(git -C "$repo" config remote.origin.url) || die "no remote.origin.url in $repo"
  # Held for this run as well as by the job, if there is one: both holds are
  # shared, and a `sync` run by hand needs one of its own.
  use "$CHECKOUT_KEY"
  ensure_root
  remove_stale_tmp_packs
  # src's commit is fetched here, on the host, over the remote this script's
  # clone came from: the agent's SSH identity. The container has none, and
  # GitHub throttles anonymous reads of this fork network (HTTP 429). One
  # commit, no history: the fork's history is ~56 GB and a build reads none.
  [ -d "$src/.git" ] || git init -q "$src"
  git -C "$src" config gc.auto 0
  # A fetch killed by SIGKILL leaves shallow.lock, and every later --depth
  # fetch then fails. Removed unless a process has it open.
  lock=$src/.git/shallow.lock
  if [ -e "$lock" ]; then
    holder=$(find /proc/[0-9]*/fd -maxdepth 1 -lname "$lock" -print -quit 2>/dev/null || true)
    if [ -n "$holder" ]; then
      die "$lock is open ($holder): another fetch is running in $src"
    fi
    echo "removing stale $lock"
    rm -f "$lock"
  fi
  # Skipped when the commit and every object of its tree are present: a killed
  # fetch of fewer than fetch.unpackLimit objects leaves loose objects without
  # the rest.
  if git -C "$src" rev-list --quiet --objects --no-walk "$commit"; then
    echo "--- $commit is complete in $src: not fetched"
  else
    echo "--- fetch $commit"
    github_read git -c protocol.version=2 -C "$src" fetch -q --depth 1 "$url" "$commit"
    # A ref, so the next fetch offers this commit as a have and receives only
    # what changed: src has no other refs, and fetch negotiates from refs.
    git -C "$src" update-ref refs/ci/fetched "$commit"
  fi
  cp "$dir/gclient" "$CHECKOUT_ROOT/.gclient"
  echo "--- checkout $CHECKOUT_ROOT -> $commit"
  "$dir/build-in-container.sh" -- /ci/container/sync.sh "$FORK_URL" "$commit"
}

wipe() {
  assert_isolated_root
  if [ ! -e "$CHECKOUT_ROOT" ]; then
    echo "nothing at $CHECKOUT_ROOT"
    return
  fi
  unused "$CHECKOUT_KEY" remove_key "$CHECKOUT_KEY"
}

wipe_out() {
  assert_isolated_root
  echo "--- removing $CHECKOUT_ROOT/src/out"
  unused "$CHECKOUT_KEY" rm -rf "$CHECKOUT_ROOT/src/out"
}

case ${1:-} in
  root) echo "$CHECKOUT_ROOT" ;;
  sync) sync ;;
  wipe) wipe ;;
  wipe-out) wipe_out ;;
  reclaim) shift; reclaim "${1:-}" ;;
  use-lock) use_lock "$CHECKOUT_KEY" ;;
  image-used) shift; image_used "${1:-}" ;;
  *)
    echo "usage: $0 root|sync|wipe|wipe-out|reclaim [GB]|use-lock|image-used <ref>" >&2
    exit 2
    ;;
esac
