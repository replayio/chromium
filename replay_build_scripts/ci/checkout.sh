#!/bin/bash
# The Chromium 157 checkout on a build agent: one directory, CHECKOUT_ROOT
# (config.sh), holding everything a 157 build keeps there.
#
#   <root>/src               the tree at the commit being built, with src/out
#   <root>/.gclient          the gclient solution (./gclient), .gclient_entries, .cipd
#   <root>/home              HOME inside the build container
#   <root>/jobs/<job id>     the current job's logs and artifacts (job.sh)
#   <root>/.replay-checkout  stamp: key, scheme, when
#
#   checkout.sh root        print <root>
#   checkout.sh sync        create <root> if it is missing, put src at this
#                           commit, gclient sync its DEPS and run the hooks
#   checkout.sh wipe-out    remove the build outputs (src/out)
#   checkout.sh wipe        remove <root>
#
# "This commit" is HEAD of the repository these scripts are in: in a job, the
# clone the pipeline step made of the commit being built.
#
# Adapted from master's and chromium-158-buildkite's checkout.sh. Removed on
# purpose: keys derived from chrome/VERSION or the environment, linking or
# adopting an existing checkout, cloning from another checkout, the shared
# .git-cache. `sync` converges from any state, so nothing is deleted on failure.
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

ensure_root() {
  local parent
  parent=$(dirname "$CHECKOUT_ROOT")
  assert_isolated_root
  mkdir -p "$parent"
  if [ ! -d "$CHECKOUT_ROOT/src/.git" ]; then
    require_free_gb "$parent" "$PREPARE_MIN_FREE_GB"
  fi
  require_free_gb "$parent" "$BUILD_MIN_FREE_GB"
  if [ ! -e "$CHECKOUT_ROOT" ]; then
    echo "--- creating $CHECKOUT_ROOT"
    mkdir "$CHECKOUT_ROOT"
    write_stamp
  fi
  if [ "$(stamp_value scheme)" != "$CHECKOUT_SCHEME" ]; then
    die "$CHECKOUT_ROOT is layout scheme '$(stamp_value scheme)', this tree builds scheme $CHECKOUT_SCHEME: checkout.sh wipe"
  fi
  mkdir -p "$CHECKOUT_ROOT/src" "$CHECKOUT_ROOT/home"
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
    git -C "$src" update-ref refs/ci157/fetched "$commit"
  fi
  cp "$dir/gclient" "$CHECKOUT_ROOT/.gclient"
  echo "--- checkout $CHECKOUT_ROOT -> $commit"
  "$dir/build-in-container.sh" -- /ci/container/sync.sh "$FORK_URL" "$commit"
}

wipe() {
  assert_isolated_root
  echo "--- removing $CHECKOUT_ROOT"
  rm -rf "$CHECKOUT_ROOT"
}

wipe_out() {
  assert_isolated_root
  echo "--- removing $CHECKOUT_ROOT/src/out"
  rm -rf "$CHECKOUT_ROOT/src/out"
}

case ${1:-} in
  root) echo "$CHECKOUT_ROOT" ;;
  sync) sync ;;
  wipe) wipe ;;
  wipe-out) wipe_out ;;
  *)
    echo "usage: $0 root|sync|wipe|wipe-out" >&2
    exit 2
    ;;
esac
