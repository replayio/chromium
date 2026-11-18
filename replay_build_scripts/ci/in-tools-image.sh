#!/bin/bash
# Run a command of the Chromium step in the tools image (tools.Dockerfile), as
# it would run on the agent:
#
#   in-tools-image.sh <key's directory> <command> [args...]
#
# - As the agent's user, with its home: files keep their owner, and git, ssh
#   and aws find the agent's identity and configuration there.
# - On the agent's network: an EC2 host's instance profile reaches aws.
# - With the key's directory, the job's workspace and the current directory at
#   the paths they have on the agent.
# - With the agent's buildkite-agent and the environment the scripts read.
#
# Named like the key's build containers, so that
# remove-stale-build-containers.sh removes one that a cancelled job left.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
root=${1:?usage: $0 <key directory> <command> [args...]}
shift

image=$("$dir/build-image.sh" --tools ref)
"$dir/build-image.sh" --tools ensure >&2

key=${root%/}
key=${key##*/}
args=(
  run --rm --init --network host
  --name "chromium-build-${key//[^A-Za-z0-9_.-]/-}-${BUILDKITE_JOB_ID:-$$}-tools"
  --user "$(id -u):$(id -g)"
  -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro
  -e HOME="$HOME" -w "$PWD"
)
declare -A mounted
for path in "$HOME" "$root" "$PWD" "${BUILDKITE_BUILD_CHECKOUT_PATH:-}"; do
  if [ -n "$path" ] && [ -z "${mounted[$path]:-}" ]; then
    mounted[$path]=1
    args+=(-v "$path:$path")
  fi
done
# Where build-in-container.sh mounts it: a dependency gclient cloned from its
# mirror borrows the mirror's objects by that path.
cache=$("$dir/checkout.sh" git-cache)
mkdir -p "$cache"
args+=(-v "$cache:/chromium-git-cache")
if [ -n "${REPLAY_LOCAL_DRIVER_DIR:-}" ]; then
  args+=(-v "$REPLAY_LOCAL_DRIVER_DIR:$REPLAY_LOCAL_DRIVER_DIR:ro")
fi
if agent=$(command -v buildkite-agent); then
  args+=(-v "$(readlink -f "$agent"):/usr/local/bin/buildkite-agent:ro")
fi
while read -r var; do
  case $var in
    BUILDKITE* | CHROMIUM_* | REPLAY_* | RECORD_REPLAY_* | RECORDREPLAY_* | AWS_* | \
      DRIVER_REVISION | LOCAL_DEVELOPER_BUILD_EXTENSION)
      args+=(-e "$var") ;;
  esac
done < <(compgen -A export)

exec docker "${args[@]}" "$image" "$@"
