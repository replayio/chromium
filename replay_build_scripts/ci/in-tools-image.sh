#!/bin/bash
# Run a command in the tools image (tools.Dockerfile), as it would run on the
# agent:
#
#   in-tools-image.sh <command> [args...]
#
# - As the agent's user, with its home: files keep their owner, and aws finds
#   the agent's identity and configuration there.
# - On the agent's network: an EC2 host's instance profile reaches aws.
# - With CHECKOUT_ROOT, the job's workspace and the current directory at the
#   paths they have on the agent.
# - With the agent's buildkite-agent and the environment the scripts read.
#
# master's in-tools-image.sh, without the DEPS mirror mount (this line has no
# .git-cache) and the local driver mount. Named CONTAINER_PREFIX-*, so that
# remove-stale-build-containers.sh removes one that a cancelled job left.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
# shellcheck source=config.sh
. "$dir/config.sh"

if [ $# -eq 0 ]; then
  echo "usage: $0 <command> [args...]" >&2
  exit 2
fi
assert_isolated_root

image=$("$dir/build-image.sh" --tools ref)
"$dir/build-image.sh" --tools ensure >&2

name="$CONTAINER_PREFIX-${BUILDKITE_JOB_ID:-manual}-$$-tools"
args=(
  run --rm --init --network host
  --name "$name"
  --user "$(id -u):$(id -g)"
  -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro
  -e HOME="$HOME" -w "$PWD"
)
declare -A mounted
for path in "$HOME" "$CHECKOUT_ROOT" "$PWD" "${BUILDKITE_BUILD_CHECKOUT_PATH:-}"; do
  if [ -n "$path" ] && [ -z "${mounted[$path]:-}" ]; then
    mounted[$path]=1
    args+=(-v "$path:$path")
  fi
done
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

# As build-in-container.sh: remove the container ourselves when the job is
# cancelled, with the docker client in the background so the trap runs at once.
for signal in INT TERM HUP; do
  # shellcheck disable=SC2064
  trap "echo 'removing $name'; docker rm --force '$name' >/dev/null 2>&1 || true; exit 1" "$signal"
done
docker "${args[@]}" "$image" "$@" &
wait $!
