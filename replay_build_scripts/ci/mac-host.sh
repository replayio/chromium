#!/usr/bin/env bash
# Build a per-version checkout on a macOS agent.
#
#   mac-host.sh                      # build the key's checkout
#   mac-host.sh --root <dir> -- ...  # run something else in it
#   mac-host.sh --check              # only check that this Mac can build
#
# The macOS counterpart of build-in-container.sh. A Mac has no image to build
# in: Xcode, the SDK and the Apple tools a Chromium build calls exist only on
# the host. So what the build image pins on Linux is pinned here, and checked
# before anything runs:
#
# - depot_tools at the revision the build image's Dockerfile names, in a clone
#   next to the keys that belongs to none of them (checkout.sh never removes
#   state whose name begins with a dot).
# - The SDK that replay_build_scripts/mac_*_args.gn name with
#   mac_sdk_official_version, from the Xcode that $REPLAY_MAC_XCODE points at.
#   Those args build with use_system_xcode = false, which reads Xcode from
#   build/mac_files/xcode_binaries in the checkout, and this links that path to
#   the pinned Xcode. In that mode gn reads Xcode's version from its plists
#   rather than asking xcodebuild, so an Xcode older than the host's macOS
#   still serves (Google's builders get the same layout from an internal CIPD
#   package, which build/mac_toolchain.py skips for everyone else).
#
# Everything else the host provides is listed in README.md, "macOS agents".
set -euo pipefail
dir=$(cd "$(dirname "$0")" && pwd)

fail() {
  echo "$(basename "$0"): $*" >&2
  exit 1
}

# The checkout's scripts assume GNU userland (find -printf, stat -c, df -BG,
# date -d, timeout, flock) and bash 4.1+ (exec {fd}>...). macOS ships BSD tools
# and bash 3.2, so the agent puts Homebrew's GNU tools and bash first on PATH.
check_host() {
  if [ "${BASH_VERSINFO[0]}" -lt 5 ]; then
    fail "bash $BASH_VERSION: the CI scripts need bash 5 (Homebrew's) first on PATH"
  fi
  local tool
  for tool in find stat df date readlink timeout; do
    "$tool" --version 2>/dev/null | grep -q GNU ||
      fail "$tool on PATH is not GNU's: put Homebrew's coreutils and findutils gnubin first on PATH"
  done
  for tool in flock git node aws buildkite-agent; do
    command -v "$tool" >/dev/null || fail "no $tool on PATH"
  done
  # The depot_tools pin's git_common.py refuses older git (Dockerfile).
  local git_version
  git_version=$(git --version | sed 's/[^0-9.]*\([0-9][0-9.]*\).*/\1/')
  printf '%s\n' 2.46 "$git_version" | sort -V -C ||
    fail "git $git_version: the depot_tools pin needs 2.46 or newer"
  xcode >/dev/null
}

# The Xcode the build reads its SDK and tools from: $REPLAY_MAC_XCODE, which
# must hold the SDK version the args files name.
xcode() {
  local app=${REPLAY_MAC_XCODE:-/Applications/Xcode-14.1.0.app} sdk
  sdk=$(sed -n 's/^ *mac_sdk_official_version *= *"\([^"]*\)".*/\1/p' \
    "$dir/../mac_arm64_args.gn" "$dir/../mac_x86_64_args.gn" | sort -u)
  if [ -z "$sdk" ] || [ "$(echo "$sdk" | wc -l)" -ne 1 ]; then
    fail "mac_arm64_args.gn and mac_x86_64_args.gn must name one mac_sdk_official_version (have: ${sdk:-none})"
  fi
  local path="$app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX$sdk.sdk"
  if [ ! -f "$app/Contents/version.plist" ] || [ ! -d "$path" ]; then
    fail "the args need MacOSX$sdk.sdk, which $app does not have. Install an Xcode that ships it (Xcode 14.1 for 13.0) and point REPLAY_MAC_XCODE at it"
  fi
  echo "$app"
}

# depot_tools at the Dockerfile's pin, cloned and bootstrapped once per pin and
# host, into $depot. The marker is written last: a clone without it is
# bootstrapped again. Not called as $(depot_tools): set -e does not apply in a
# command substitution, and a failed fetch would then still write the marker.
depot_tools() {
  local rev base d lock
  rev=$(sed -n 's/^ARG DEPOT_TOOLS_REVISION=//p' "$dir/Dockerfile")
  [ -n "$rev" ] || fail "no DEPOT_TOOLS_REVISION in $dir/Dockerfile"
  base=$("$dir/checkout.sh" checkouts-dir)/.depot_tools
  d=$base/$rev
  depot=$d
  if [ -f "$d/.replay-revision" ]; then
    return
  fi
  mkdir -p "$base"
  exec {lock}>"$base/.lock"
  flock "$lock"
  if [ ! -d "$d" ]; then
    local tmp
    # What a clone that failed or was cancelled left; the lock is ours.
    rm -rf "$base"/.clone.*
    tmp=$(mktemp -d "$base/.clone.XXXXXX")
    git init -q "$tmp"
    git -C "$tmp" remote add origin https://chromium.googlesource.com/chromium/tools/depot_tools.git
    git -C "$tmp" fetch -q --depth 1 origin "$rev" >&2
    git -C "$tmp" checkout -q FETCH_HEAD
    touch "$tmp/.disable_auto_update"
    mv "$tmp" "$d"
  fi
  # In place, not before the move: the bootstrap records where it put python.
  # With depot_tools on PATH, as in the Dockerfile: its gsutil step runs
  # luci-auth from there.
  (
    cd "$d"
    export DEPOT_TOOLS_UPDATE=0 PATH=$d:$PATH
    ./ensure_bootstrap && ./gclient --version && ./cipd --version &&
      test -f python3_bin_reldir.txt
  ) >&2
  echo "$rev" > "$d/.replay-revision"
  exec {lock}>&-
}

root=""
cmd=()
while [ $# -gt 0 ]; do
  case $1 in
    --check) check_host; echo "$(basename "$0"): this Mac can build ($(xcode))"; exit 0 ;;
    --root) root=${2:?--root needs a directory}; shift 2 ;;
    --) shift; cmd=("$@"); break ;;
    *) echo "usage: $0 [--check] [--root <dir>] [-- <command>]" >&2; exit 1 ;;
  esac
done
check_host
if [ -z "$root" ]; then
  root=$("$dir/checkout.sh" require)
fi
if [ ${#cmd[@]} -eq 0 ]; then
  cmd=(node build.js)
fi

app=$(xcode)
depot_tools

# Held until this run ends: no reclaim removes the key under it (checkout.sh).
key=${root%/}
key=${key##*/}
exec {use}>"$("$dir/checkout.sh" use-lock "$key")"
flock -s "$use"

# A prepare runs this before the tree has its build directory.
if [ -d "$root/src/build" ]; then
  link=$root/src/build/mac_files/xcode_binaries
  # A directory there is someone's own Xcode (build/mac_toolchain.py puts
  # Google's there), which ln would link into rather than replace.
  if [ -e "$link" ] && [ ! -L "$link" ]; then
    fail "$link is a directory, not the link to the pinned Xcode: move it away"
  fi
  mkdir -p "$(dirname "$link")"
  ln -sfn "$app" "$link"
fi

cache=$("$dir/checkout.sh" git-cache)
mkdir -p "$cache"
export GIT_CACHE_PATH=$cache
export DEPOT_TOOLS_UPDATE=0
export DEVELOPER_DIR=$app/Contents/Developer
export PATH=$depot:$PATH
# The EngFlow cert and key stay where the job put them, outside the checkout.
if [ -n "${RBE_service:-}" ] &&
  { [ -z "${RBE_tls_client_auth_cert:-}" ] || [ -z "${RBE_tls_client_auth_key:-}" ]; }; then
  echo "$(basename "$0"): RBE_service is set but no EngFlow cert/key; compiles would fall back to this machine" >&2
fi

cd "$root/src"
echo "\$ ${cmd[*]}   (in $PWD, Xcode $app, depot_tools $depot)"
exec "${cmd[@]}"
