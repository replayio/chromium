#!/bin/bash
# Upload the build artifacts of this key's checkout.
#
# upload_build_artifacts.mjs reads out/Release and the backend checkout relative
# to the current directory, so it used to be a pipeline command that worked
# because the pipeline had cd'd into the one shared checkout. Here it runs in the
# key's checkout, with that key's backend, and from the tree's own copy of the
# script -- the tree is at the commit being built by then (checkout.sh sync).
#
# It runs on the agent, not in the build image: it needs the AWS identity and
# the buildkite-agent binary, and it writes into the job's workspace
# (BUILDKITE_BUILD_CHECKOUT_PATH), which is where artifact_paths look.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
root=$("$dir/checkout.sh" require)

cd "$root/src"
RECORD_REPLAY_BACKEND_DIR="$root/backend" \
  exec node replay_build_scripts/upload_build_artifacts.mjs "$@"
