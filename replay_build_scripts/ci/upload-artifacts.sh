#!/bin/bash
# Upload the build artifacts of this key's checkout.
#
# upload_build_artifacts.mjs reads out/Release and the backend checkout relative
# to the current directory, so it used to be a pipeline command that worked
# because the pipeline had cd'd into the one shared checkout. Here it runs in the
# key's checkout, with that key's backend, and from the tree's own copy of the
# script -- the tree is at the commit being built by then (checkout.sh sync).
#
# It runs in the tools image, not the build image: it needs the agent's AWS
# identity and buildkite-agent, and it writes into the job's workspace
# (BUILDKITE_BUILD_CHECKOUT_PATH), which is where artifact_paths look. On a Mac,
# which has no images, it runs on the host, which has node, aws and
# buildkite-agent (mac-host.sh checks).
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
root=$("$dir/checkout.sh" root)

cd "$root/src"
export RECORD_REPLAY_BACKEND_DIR="$root/backend"
if [ "$(uname -s)" = Darwin ]; then
  exec node replay_build_scripts/upload_build_artifacts.mjs "$@"
fi
exec "$dir/in-tools-image.sh" "$root" node replay_build_scripts/upload_build_artifacts.mjs "$@"
