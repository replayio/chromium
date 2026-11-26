#!/bin/bash
# Run a command in this version's build image, on its checkout.
#
#   build-in-container.sh [--engflow] [--require-synced] -- <command>...
#
# What the command sees of the host:
#   /chromium   CHECKOUT_ROOT (config.sh), read-write; HOME is /chromium/home
#   /ci         this directory, read-only: the scripts of the commit being built
#   /engflow    with --engflow only: the EngFlow mTLS client cert and key
#               ($ENGFLOW_CERT_FILE, $ENGFLOW_KEY_FILE), read-only
# depot_tools is the image's. No other checkout, depot_tools clone or cache of
# the host is mounted.
#
# From master's and chromium-158-buildkite's build-in-container.sh.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
# shellcheck source=config.sh
. "$dir/config.sh"

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

assert_isolated_root
if [ ! -d "$CHECKOUT_ROOT/src" ] || [ ! -d "$CHECKOUT_ROOT/home" ]; then
  die "no checkout at $CHECKOUT_ROOT: checkout.sh sync"
fi
# The sync itself runs without this: it is what makes the checkout usable.
if [ "$require_synced" = 1 ] && [ ! -e "$CHECKOUT_SYNCED" ]; then
  die "no sync has finished in $CHECKOUT_ROOT: run checkout.sh sync before building in it"
fi

image=$("$dir/build-image.sh" ref)
"$dir/build-image.sh" ensure >&2

# The prefix carries the key, so it is what remove-stale-build-containers.sh and
# the handlers below remove by without reaching another version's containers.
# The pid keeps one job's successive containers apart.
name="$CONTAINER_PREFIX-${BUILDKITE_JOB_ID:-manual}-$$"

args=(
  # --init: PID 1 forwards SIGTERM, so a removed container's build stops
  # (replayio/chromium#1540).
  run --name "$name" --rm --init
  -v "$CHECKOUT_ROOT:/chromium"
  -v "$dir:/ci:ro"
  -w /chromium/src
  --user "$(id -u):$(id -g)"
  -e HOME=/chromium/home
  # Whatever else runs on the agent gets the CPU first, and the kernel's OOM
  # killer takes this container before the agent or the docker daemon.
  --cpu-shares 128
  --oom-score-adj 500
)
for var in BUILDKITE BUILDKITE_BRANCH BUILDKITE_PIPELINE_DEFAULT_BRANCH \
           LOCAL_DEVELOPER_BUILD_EXTENSION DRIVER_REVISION \
           REPLAY_BUILD_MAX_FAILURES REPLAY_SISO_LOCAL_JOBS REPLAY_SISO_REMOTE_JOBS \
           REPLAY_SISO_CONFIG; do
  if [ -n "${!var:-}" ]; then
    args+=(-e "$var")
  fi
done

if [ "$engflow" = 1 ]; then
  cert=${ENGFLOW_CERT_FILE:?ENGFLOW_CERT_FILE is not set}
  key=${ENGFLOW_KEY_FILE:?ENGFLOW_KEY_FILE is not set}
  if [ ! -s "$cert" ] || [ ! -s "$key" ]; then
    die "EngFlow client cert or key is missing or empty ($cert, $key)"
  fi
  # siso takes its mTLS client pair from these two variables.
  args+=(
    -v "$(readlink -f "$cert"):/engflow/client.crt:ro"
    -v "$(readlink -f "$key"):/engflow/client.key:ro"
    -e RBE_tls_client_auth_cert=/engflow/client.crt
    -e RBE_tls_client_auth_key=/engflow/client.key
  )
fi

# A container that outlives its job (the job was killed before the handlers
# below ran) still ends, one job timeout after it started, instead of building
# on next to whatever the agent runs afterwards.
if [ -n "${REPLAY_JOB_TIMEOUT_MINUTES:-}" ]; then
  cmd=(timeout --signal=TERM --kill-after=60s "${REPLAY_JOB_TIMEOUT_MINUTES}m" "${cmd[@]}")
fi

# Remove the container ourselves when the job is cancelled, rather than rely
# on the docker client passing the signal on before it is killed itself.
for signal in INT TERM HUP; do
  # shellcheck disable=SC2064
  trap "echo 'removing $name'; docker rm --force '$name' >/dev/null 2>&1 || true; exit 1" "$signal"
done

echo "\$ docker ${args[*]} $image ${cmd[*]}"
# In the background and waited for: bash runs a trap only once the foreground
# child has exited, so the cleanup would otherwise wait for the build.
docker "${args[@]}" "$image" "${cmd[@]}" &
wait $!
