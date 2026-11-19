#!/bin/bash
# One Buildkite job: build Chromium at this commit, upload the logs and,
# if it built, the runtime. Runs on the agent, from the job's own clone of
# this directory (.buildkite/pipeline.yml); "this commit" is that clone's HEAD.
#
# Environment (set on the Buildkite build, not in pipeline.yml: a step's env
# would override it):
#   REPLAY_BUILD_MAX_FAILURES   failed steps before the build stops.
#                               0 (default): keep going, report every failure.
#                               1: stop at the first error.
#   REPLAY_SISO_LOCAL_JOBS      parallel local steps (default: siso's, the CPU count)
#   REPLAY_SISO_CONFIG          siso -config names (default: none)
# From the pipeline step:
#   ENGFLOW_CERT_FILE, ENGFLOW_KEY_FILE   EngFlow mTLS client cert and key
#   REPLAY_JOB_TIMEOUT_MINUTES            the step's timeout
#
# Artifacts, all uploaded from <root>/jobs/<job id>:
#   logs/build.log.gz           the whole build output
#   logs/failed-steps.txt       one line per failed step
#   logs/siso_failure_summary   siso's own report of each failure
#   logs/siso_output.gz, logs/siso_failed_commands.sh
#   build_id/linux/x86_64/build_id, build_id/linux/x86_64/<build id>.tar.xz
#   build_id/linux/x86_64/<build id>.symbols.tgz   chrome's symbols, in the
#                               archive master uploads to S3 symbols/
#   build_id/linux/x86_64/backend_rev   the backend commit this build embeds a
#                               driver of, for the steps that test it
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
# shellcheck source=config.sh
. "$dir/config.sh"

job=${BUILDKITE_JOB_ID:?job.sh runs as a Buildkite job}
export REPLAY_BUILD_MAX_FAILURES=${REPLAY_BUILD_MAX_FAILURES:-0}

# Held until the job ends: another job's reclaim, which removes the least
# recently used keys when the volume runs low, leaves this one alone between the
# steps below (checkout.sh).
exec {key_use}>"$("$dir/checkout.sh" use-lock)"
flock -s "$key_use"

"$dir/remove-stale-build-containers.sh"
"$dir/checkout.sh" sync

# Driver and backend come from this commit's REPLAY_BACKEND_REV, however the
# build was started: not from build env, never the latest driver. Read from the
# checkout, which has the whole tree -- this script's own clone has
# replay_build_scripts/ci alone. The file is "<commit> [<branch>]".
rev_file=$CHECKOUT_ROOT/src/REPLAY_BACKEND_REV
[ -f "$rev_file" ] || die "$rev_file is missing: the commit being built pins no backend"
BACKEND_REV=$(awk 'NR == 1 { print $1 }' "$rev_file")
[ -n "$BACKEND_REV" ] || die "no commit in $rev_file"
export DRIVER_REVISION=${BACKEND_REV:0:12}
echo "backend $BACKEND_REV, driver $DRIVER_REVISION"

# This job's logs and artifacts; an earlier job's are dropped.
assert_isolated_root
rm -rf "$CHECKOUT_ROOT/jobs"
out=$CHECKOUT_ROOT/jobs/$job
logs=$out/logs
mkdir -p "$logs"
build_started=$out/.build-started
touch "$build_started"

echo "--- :hammer: build"
status=0
"$dir/build-in-container.sh" --engflow -- node build.cjs 2>&1 | tee "$logs/build.log" || status=$?

# siso's own logs, if this build wrote them.
for name in siso_failure_summary siso_output siso_failed_commands.sh; do
  file=$CHECKOUT_ROOT/src/out/Release/$name
  if [ -f "$file" ] && [ "$file" -nt "$build_started" ]; then
    cp "$file" "$logs/$name"
  fi
done
# A failed step is a line starting with "FAILED: ", from siso and ninja alike.
sed -e 's/\x1b\[[0-9;]*m//g' "$logs/build.log" | sed -n '/^FAILED: /p' > "$logs/failed-steps.txt"
failed=$(wc -l < "$logs/failed-steps.txt")
gzip "$logs/build.log"
if [ -f "$logs/siso_output" ]; then
  gzip "$logs/siso_output"
fi
( cd "$out" && buildkite-agent artifact upload "logs/**" )

if [ "$status" != 0 ]; then
  echo "^^^ +++"
  echo "+++ build failed (exit $status): $failed failed steps"
  head -n 200 "$logs/failed-steps.txt"
  echo "artifacts: logs/failed-steps.txt, logs/siso_failure_summary, logs/build.log.gz"
  exit "$status"
fi

echo "--- :package: package"
"$dir/build-in-container.sh" -- /ci/container/package.sh "/chromium/jobs/$job/build_id/linux/x86_64"
# With the tree's node: the image's is too old for the script's modules.
"$dir/build-in-container.sh" -- /chromium/src/third_party/node/linux/node-linux-x64/bin/node \
  replay_build_scripts/symbols_archive.mjs "/chromium/jobs/$job/build_id/linux/x86_64"
# Next to build_id: the backend that steps testing this build must use.
echo "$BACKEND_REV" > "$out/build_id/linux/x86_64/backend_rev"
( cd "$out" && buildkite-agent artifact upload "build_id/linux/x86_64/**" )
echo "build id: $(cat "$out/build_id/linux/x86_64/build_id")"
