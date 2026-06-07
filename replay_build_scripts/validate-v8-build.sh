#!/usr/bin/env bash
# Standalone V8 (d8) compile-validation for the replayio/chromium-v8 fork (v8-m151).
#
# Purpose: validate that the Replay V8 instrumentation actually COMPILES on a real
# toolchain, WITHOUT needing the (blocked) macOS Chromium build. A standalone d8
# build compiles the exact v8-m151 files — d8 + inspector + maglev + heap +
# compiler + torque (weak-ref.tq) + the src/replay/* new files — i.e. essentially
# all of the V8 fork's compile surface. Errors here are the subtle type/template
# issues static review can't catch.
#
# Runs on a Linux Buildkite agent (os=linux, queue=runtime). Self-contained: uses
# its own work dir and its own depot_tools/gclient V8 checkout, so it does NOT
# touch the agent's chromium Replay tree (whose gclient is intentionally disabled).
#
# Env overrides:
#   V8_VALIDATE_DIR  work dir (default: $HOME/v8-validate)
#   V8_REV           fork commit to build (default: the v8-m151 tip below)
#   V8_FORK_URL      fork remote (default: public HTTPS chromium-v8)
#   V8_TARGET_CPU    x64 (default) | arm64
#   V8_NINJA_TARGETS ninja targets (default: d8 — pulls in inspector/maglev/etc.)
set -euo pipefail

WORK="${V8_VALIDATE_DIR:-$HOME/v8-validate}"
V8_REV="${V8_REV:-cee65465270c49a470c51a9e518edcbc0b2ca630}"
V8_FORK_URL="${V8_FORK_URL:-https://github.com/replayio/chromium-v8.git}"
V8_TARGET_CPU="${V8_TARGET_CPU:-x64}"
V8_NINJA_TARGETS="${V8_NINJA_TARGETS:-d8}"
OUT="out/${V8_TARGET_CPU}.release"

log() { echo "+++ [validate-v8] $*"; }

mkdir -p "$WORK"
cd "$WORK"

# 1. depot_tools (gclient/gn/ninja/fetch). Reuse if already on PATH.
if ! command -v gclient >/dev/null 2>&1; then
  if [ ! -d "$WORK/depot_tools" ]; then
    log "cloning depot_tools"
    git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "$WORK/depot_tools"
  fi
  export PATH="$WORK/depot_tools:$PATH"
fi
export DEPOT_TOOLS_UPDATE=0
export GIT_TERMINAL_PROMPT=0

# 2. Standalone V8 checkout pinned at our fork revision.
#    With managed:False, gclient does NOT manage the solution's own git checkout —
#    so we clone + checkout the fork commit FIRST, then `gclient sync` reads that
#    checkout's DEPS and fetches V8's pinned build deps. (Order matters.)
mkdir -p "$WORK/build" && cd "$WORK/build"
cat > .gclient <<EOF
solutions = [
  {
    "name": "v8",
    "url": "${V8_FORK_URL}",
    "managed": False,
    "deps_file": "DEPS",
    "custom_deps": {},
  },
]
target_os = ["host"]
EOF

if [ ! -d v8/.git ]; then
  log "cloning fork"
  git clone --filter=blob:none "$V8_FORK_URL" v8
fi
cd "$WORK/build/v8"
git remote set-url origin "$V8_FORK_URL"   # keep solution's remote == fork (idempotent on reuse)
log "checkout fork ${V8_REV}"
git fetch origin v8-m151
git checkout --force "$V8_REV"

cd "$WORK/build"
log "gclient sync (fetches V8's DEPS-pinned build deps + runs hooks: clang/gn/etc.)"
# -D prunes removed deps; --force --reset recover cleanly from a partial prior run;
# hooks run by default (no --nohooks) -> pull clang & the gn binary into buildtools.
gclient sync -D --force --reset

cd "$WORK/build/v8"
# Fail early + clearly if the DEPS hooks didn't land a working gn (otherwise the
# build dies later with a confusing 'gn: command not found').
if ! gn --version >/dev/null 2>&1; then
  log "ERROR: 'gn' not working after gclient sync — V8 DEPS hooks (gn/clang download) likely failed"
  exit 1
fi

# 3. Configure for a COMPILE-validation build:
#    - release, no remoteexec/RBE (local toolchain only)
#    - treat_warnings_as_errors=false so real ERRORS aren't drowned by warning noise
#    - keep defaults for sandbox / pointer-compression / maglev / inspector so the
#      same code paths as the chromium embedding are exercised.
log "gn gen ${OUT}"
gn gen "$OUT" --args="is_debug=false target_cpu=\"${V8_TARGET_CPU}\" v8_enable_backtrace=true treat_warnings_as_errors=false use_remoteexec=false v8_enable_verify_heap=false symbol_level=1"

# 4. Compile. Tee to a log and surface a concise error summary at the end.
log "ninja -C ${OUT} ${V8_NINJA_TARGETS}"
set +e
ninja -C "$OUT" $V8_NINJA_TARGETS 2>&1 | tee "$WORK/v8-build.log"
rc=${PIPESTATUS[0]}
set -e

echo ""
if [ "$rc" -eq 0 ]; then
  log "BUILD OK — v8-m151 (${V8_REV}) compiled d8 cleanly for ${V8_TARGET_CPU}"
else
  log "BUILD FAILED (rc=$rc). First errors:"
  grep -nE "error:|fatal error:|undefined reference|no member named|no matching|note: in instantiation" "$WORK/v8-build.log" | head -60 || true
fi
exit $rc
