#!/bin/bash
# Run a command in this key's checkout on a macOS agent: the macOS counterpart
# of build-in-container.sh, with the same arguments.
#
#   mac-host.sh [--engflow] [--require-synced] -- <command>...
#
# A Mac has no image to build in: the SDK and the Apple tools a Chromium build
# calls exist only on the host. So what the build image pins on Linux is pinned
# here, and checked before anything runs:
#
# - depot_tools at the revision the build image's Dockerfile names, in a clone
#   beside the keys that belongs to none of them (a name beginning with a dot is
#   not a key, and reclaim never removes one).
# - Xcode, as build/mac_files/xcode_binaries in the checkout, made from Apple's
#   Command Line Tools on this Mac. The args build with use_system_xcode =
#   false, which reads the SDK and the tools from there, and the version from
#   its Contents/version.plist instead of asking xcodebuild, which the Command
#   Line Tools do not have. Google's builders get that layout from an internal
#   CIPD package of Xcode, whose contents build/xcode_binaries.yaml lists, and
#   build/mac_toolchain.py skips it for everyone else. The Command Line Tools
#   have all of it except the Metal compiler, so ANGLE compiles its internal
#   shaders when Chromium starts instead (mac/angle-runtime-shaders.patch,
#   applied by container/sync.sh). They install without an Apple ID, and come
#   with the EC2 macOS images (replayio/backend pulumi/build-machines, which
#   updates them on a Mac's first boot).
#
# Everything else the agent provides is in README.md, "macOS agents".
set -euo pipefail
dir=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=config.sh
. "$dir/config.sh"

# Where `xcode-select --install` and the EC2 images put them.
readonly CLT=/Library/Developer/CommandLineTools
# What build/xcode_binaries.yaml lists that the Command Line Tools do not have,
# and the build does without: the Metal toolchain (see above), symbolication
# and notarization tools, and Xcode's license files.
readonly XCODE_OPTIONAL='
Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/metal/
Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/air-lld
Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/metal
Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/metallib
Contents/Developer/usr/bin/atos
Contents/Developer/usr/bin/notarytool
Contents/Resources/LicenseInfo.plist
Contents/Resources/en.lproj/License.rtf
Contents/SharedFrameworks/CoreSymbolicationDT.framework/
Contents/SharedFrameworks/DebugSymbolsDT.framework/
Contents/SharedFrameworks/MallocStackLoggingDT.framework/
Contents/SharedFrameworks/SymbolicationDT.framework/
Contents/version.plist
'

fail() {
  echo "$(basename "$0"): $*" >&2
  exit 1
}

# The scripts here assume GNU userland (find -printf, stat -c, df -BG,
# readlink -m, timeout --foreground, flock) and bash 4.1+ (exec {fd}>...).
# macOS ships BSD tools and bash 3.2, so the agent puts Homebrew's GNU tools and
# bash first on PATH.
check_host() {
  if [ "${BASH_VERSINFO[0]}" -lt 5 ]; then
    fail "bash $BASH_VERSION: the CI scripts need bash 5 (Homebrew's) first on PATH"
  fi
  local tool
  for tool in find stat df date readlink timeout; do
    "$tool" --version 2>/dev/null | grep -q GNU ||
      fail "$tool on PATH is not GNU's: put Homebrew's coreutils and findutils gnubin first on PATH"
  done
  for tool in flock git node python3 rsync aws buildkite-agent; do
    command -v "$tool" >/dev/null || fail "no $tool on PATH"
  done
  # The depot_tools pin's git_common.py refuses older git (Dockerfile).
  local git_version
  git_version=$(git --version | sed 's/[^0-9.]*\([0-9][0-9.]*\).*/\1/')
  printf '%s\n' 2.46 "$git_version" | sort -V -C ||
    fail "git $git_version: the depot_tools pin needs 2.46 or newer"
  [ -d "$CLT/SDKs/MacOSX.sdk" ] ||
    fail "no $CLT/SDKs/MacOSX.sdk: install Apple's Command Line Tools (xcode-select --install)"
}

# depot_tools at the Dockerfile's pin, cloned and bootstrapped once per pin and
# host, into $depot. The marker is written last: a clone without it is
# bootstrapped again. Not called as $(depot_tools): set -e does not apply in a
# command substitution, and a failed fetch would then still write the marker.
depot_tools() {
  local rev base d lock tmp
  rev=$(sed -n 's/^ARG DEPOT_TOOLS_REVISION=//p' "$dir/Dockerfile")
  [ -n "$rev" ] || fail "no DEPOT_TOOLS_REVISION in $dir/Dockerfile"
  base=$CHECKOUTS_DIR/.depot_tools
  d=$base/$rev
  depot=$d
  if [ -f "$d/.replay-revision" ]; then
    return
  fi
  mkdir -p "$base"
  exec {lock}>"$base/.lock"
  flock "$lock"
  if [ ! -d "$d" ]; then
    # What a clone that failed or was cancelled left; the lock is ours.
    rm -rf "$base"/.clone.*
    tmp=$(mktemp -d "$base/.clone.XXXXXX")
    git init -q "$tmp"
    git -C "$tmp" remote add origin https://chromium.googlesource.com/chromium/tools/depot_tools.git
    github_read git -C "$tmp" fetch -q --depth 1 origin "$rev" >&2
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

plist_value() {
  plutil -extract "$2" raw -o - "$1"
}

# build/mac_files/xcode_binaries, laid out as Xcode.app is and holding what
# build/xcode_binaries.yaml lists, from the Command Line Tools: their tools for
# Xcode's toolchain directory, their SDK as MacOSX.sdk and as MacOSX<version>.sdk,
# and a Contents/version.plist that names their version, which gn reads as
# Xcode's. Made again whenever the Command Line Tools or the list change, and
# into a new directory that replaces the old one only once it is complete.
#
# Sets SDK_VERSION and SDK_BUILD_VERSION, which build.cjs gives gn.
install_xcode() {
  local src=$1 dest list sdk clt_version stamp tmp kind path from to missing=()
  dest=$src/build/mac_files/xcode_binaries
  list=$src/build/xcode_binaries.yaml
  [ -f "$list" ] || fail "no $list"
  sdk=$(readlink -f "$CLT/SDKs/MacOSX.sdk")
  SDK_VERSION=$(plist_value "$sdk/SDKSettings.plist" Version)
  SDK_BUILD_VERSION=$(plist_value "$sdk/System/Library/CoreServices/SystemVersion.plist" ProductBuildVersion)
  clt_version=$(pkgutil --pkg-info=com.apple.pkg.CLTools_Executables | sed -n 's/^version: //p')
  [ -n "$SDK_VERSION" ] && [ -n "$SDK_BUILD_VERSION" ] && [ -n "$clt_version" ] ||
    fail "cannot read the versions of the Command Line Tools in $CLT"
  stamp="clt=$clt_version sdk=$SDK_VERSION ($SDK_BUILD_VERSION) list=$(sha256sum "$list" | cut -c1-16)"
  if [ "$(cat "$dest/.replay-xcode" 2>/dev/null || true)" = "$stamp" ]; then
    return
  fi
  # A directory without the stamp is someone's own Xcode (build/mac_toolchain.py
  # puts Google's there), which this would replace.
  if [ -e "$dest" ] && [ ! -f "$dest/.replay-xcode" ]; then
    fail "$dest is not one this script made: move it away"
  fi
  echo "--- Xcode from the Command Line Tools: $stamp"
  mkdir -p "$(dirname "$dest")"
  rm -rf "$dest".new.*
  tmp=$(mktemp -d "$dest.new.XXXXXX")
  while read -r kind path; do
    to=$tmp/$path
    case $path in
      Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/)
        mkdir -p "$to"
        rsync -a "$sdk/" "$to/MacOSX.sdk/"
        ln -s MacOSX.sdk "$to/MacOSX$SDK_VERSION.sdk"
        continue
        ;;
      Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/*)
        from=$CLT/usr/${path#Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/} ;;
      Contents/Developer/usr/*)
        from=$CLT/usr/${path#Contents/Developer/usr/} ;;
      *) from= ;;
    esac
    if [ -n "$from" ] && [ "$kind" = dir ] && [ -d "$from" ]; then
      mkdir -p "$to"
      rsync -a "$from/" "$to/"
    elif [ -n "$from" ] && [ "$kind" = file ] && [ -e "$from" ]; then
      mkdir -p "$(dirname "$to")"
      # The file, not a link: some are links to tools this does not copy.
      cp -pL "$from" "$to"
    elif ! grep -qxF "$path" <<<"$XCODE_OPTIONAL"; then
      missing+=("$path")
    fi
  done < <(sed -n 's/^ *- \(file\|dir\): /\1 /p' "$list")
  if [ ${#missing[@]} -gt 0 ]; then
    rm -rf "$tmp"
    fail "the Command Line Tools in $CLT lack what build/xcode_binaries.yaml lists: ${missing[*]}"
  fi
  cat > "$tmp/Contents/version.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleShortVersionString</key>
	<string>$(echo "$clt_version" | cut -d . -f 1-2)</string>
	<key>ProductBuildVersion</key>
	<string>CLT-$clt_version</string>
</dict>
</plist>
EOF
  echo "$stamp" > "$tmp/.replay-xcode"
  rm -rf "$dest"
  mv "$tmp" "$dest"
}

engflow=0
require_synced=0
cmd=()
while [ $# -gt 0 ]; do
  case $1 in
    --engflow) engflow=1; shift ;;
    --require-synced) require_synced=1; shift ;;
    --) shift; cmd=("$@"); break ;;
    *) break ;;
  esac
done
if [ ${#cmd[@]} -eq 0 ]; then
  echo "usage: $0 [--engflow] [--require-synced] -- <command>..." >&2
  exit 2
fi

check_host
assert_isolated_root
if [ ! -d "$CHECKOUT_ROOT/src" ] || [ ! -d "$CHECKOUT_ROOT/home" ]; then
  die "no checkout at $CHECKOUT_ROOT: checkout.sh sync"
fi
if [ "$require_synced" = 1 ] && [ ! -e "$CHECKOUT_SYNCED" ]; then
  die "no sync has finished in $CHECKOUT_ROOT: run checkout.sh sync before building in it"
fi
depot_tools

# The first sync phase runs before src has its tree.
if [ -f "$CHECKOUT_ROOT/src/build/xcode_binaries.yaml" ]; then
  install_xcode "$CHECKOUT_ROOT/src"
  export REPLAY_MAC_SDK_VERSION=$SDK_VERSION REPLAY_MAC_SDK_BUILD_VERSION=$SDK_BUILD_VERSION
fi

# What the build image sets up: its HOME, depot_tools on PATH, no auto-gc
# (/etc/gitconfig there), and the key's directory (container/sync.sh).
export HOME=$CHECKOUT_ROOT/home CHROMIUM_ROOT=$CHECKOUT_ROOT
export DEPOT_TOOLS_UPDATE=0 PATH=$depot:$PATH
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=gc.auto GIT_CONFIG_VALUE_0=0
# build.cjs: hold this build to what it checks in the build image.
export REPLAY_CI_HOST_BUILD=1
if [ "$engflow" = 1 ]; then
  cert=${ENGFLOW_CERT_FILE:?ENGFLOW_CERT_FILE is not set}
  key=${ENGFLOW_KEY_FILE:?ENGFLOW_KEY_FILE is not set}
  if [ ! -s "$cert" ] || [ ! -s "$key" ]; then
    die "EngFlow client cert or key is missing or empty ($cert, $key)"
  fi
  RBE_tls_client_auth_cert=$(readlink -f "$cert")
  RBE_tls_client_auth_key=$(readlink -f "$key")
  export RBE_tls_client_auth_cert RBE_tls_client_auth_key
fi
if [ -n "${REPLAY_JOB_TIMEOUT_MINUTES:-}" ]; then
  cmd=(timeout --signal=TERM --kill-after=60s "${REPLAY_JOB_TIMEOUT_MINUTES}m" "${cmd[@]}")
fi

cd "$CHECKOUT_ROOT/src"
echo "\$ ${cmd[*]}   (in $PWD, depot_tools $depot)"
exec "${cmd[@]}"
