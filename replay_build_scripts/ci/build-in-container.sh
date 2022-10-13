#!/bin/bash
# Build a per-version checkout in the image this repo describes.
#
#   build-in-container.sh                      # build the key's checkout
#   build-in-container.sh --root <dir> -- ...  # run something else in it
#
# It replaces //buildLinux.mjs, which ran `docker run` on the hand-built
# chromium-build-new image with $HOME/chromium and $HOME/depot_tools mounted
# into it: the container was where the tools came from, but the state the build
# wrote, and the depot_tools it used, were the agent's and shared by everything
# that built there. Here the only writable mount is one key's directory
# (checkout.sh), depot_tools is the pin inside the image, and the EngFlow client
# cert and key are mounted read-only from outside the checkout, so that a job
# cannot leave them behind in it.
#
# The mount point is /chromium for every key, so gn, ninja and reclient see the
# same paths they always have and nothing rebuilds because a directory moved.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")

root=""
cmd=()
while [ $# -gt 0 ]; do
  case $1 in
    --root) root=${2:?--root needs a directory}; shift 2 ;;
    --) shift; cmd=("$@"); break ;;
    *) echo "usage: $0 [--root <dir>] [-- <command>]" >&2; exit 1 ;;
  esac
done
if [ -z "$root" ]; then
  root=$("$dir/checkout.sh" require)
fi
if [ ${#cmd[@]} -eq 0 ]; then
  cmd=(node build.js)
fi
mkdir -p "$root/home"
cache=$("$dir/checkout.sh" git-cache)
mkdir -p "$cache"

image=$("$dir/build-image.sh" ref)
"$dir/build-image.sh" ensure >&2

# Named after the version and the job: cancelling the job can then remove the
# container, and the sweep for orphans can leave other versions' builds alone.
# --init: the image's command runs under a shell, which as PID 1 would ignore
# SIGTERM, and a cancelled job's build then kept running in the checkout
# (replayio/chromium#1540).
key=${root%/}
key=${key##*/}
name="chromium-build-${key//[^A-Za-z0-9_.-]/-}-${BUILDKITE_JOB_ID:-$$}"

args=(
  run --name "$name" --rm --init
  -v "$root:/chromium"
  -w /chromium/src
  # gclient's shared mirrors of the DEPS repositories, which a sync in here
  # clones the dependencies from (GIT_CACHE_PATH=/chromium-git-cache). It holds
  # nothing version-specific, so every key sees the same one.
  -v "$cache:/chromium-git-cache"
  -e GIT_CACHE_PATH=/chromium-git-cache
  # The agent and the image's ubuntu are both uid 1000; passing it explicitly
  # means the files a build writes belong to whoever ran it either way.
  --user "$(id -u):$(id -g)"
  -e HOME=/chromium/home
)
for var in BUILDKITE BUILDKITE_BRANCH BUILDKITE_PIPELINE_DEFAULT_BRANCH \
           LOCAL_DEVELOPER_BUILD_EXTENSION DRIVER_REVISION \
           RBE_service RBE_service_no_auth RBE_use_application_default_credentials; do
  if [ -n "${!var:-}" ]; then
    args+=(-e "$var")
  fi
done

# The cert and key stay outside the checkout, read-only, for this run only.
cert=${RBE_tls_client_auth_cert:-${ENGFLOW_CERT_FILE:-}}
key=${RBE_tls_client_auth_key:-${ENGFLOW_KEY_FILE:-}}
if [ -n "$cert" ] && [ -n "$key" ]; then
  args+=(
    -v "$(readlink -f "$cert"):/engflow/engflow.crt:ro"
    -v "$(readlink -f "$key"):/engflow/engflow.key:ro"
    -e RBE_tls_client_auth_cert=/engflow/engflow.crt
    -e RBE_tls_client_auth_key=/engflow/engflow.key
  )
elif [ -n "${RBE_service:-}" ]; then
  echo "$(basename "$0"): RBE_service is set but no EngFlow cert/key; compiles would fall back to this machine" >&2
fi

if [ -n "${REPLAY_LOCAL_DRIVER_DIR:-}" ]; then
  driver=$(readlink -f "$REPLAY_LOCAL_DRIVER_DIR")
  args+=(-v "$driver:$driver:ro" -e "REPLAY_LOCAL_DRIVER_DIR=$driver")
fi

# Remove the container ourselves when the job is cancelled, rather than rely on
# the docker client passing the signal on before it is killed itself.
for signal in INT TERM HUP; do
  # shellcheck disable=SC2064
  trap "echo 'removing $name'; docker rm --force '$name' >/dev/null 2>&1 || true; exit 1" "$signal"
done

echo "\$ docker ${args[*]} $image ${cmd[*]}"
# In the background and waited for, not in the foreground: bash runs a trap
# only once the foreground child has exited, so a cancelled job's cleanup would
# wait for the build it is trying to stop.
docker "${args[@]}" "$image" "${cmd[@]}" &
wait $!
