#!/bin/bash
# In the build image: archive the runtime that out/Release holds.
#
#   package.sh <dest dir>
#     <dest>/build_id            the build id
#     <dest>/<build id>.tar.xz   replay-chromium/
#
# The archive is the one master's upload_build_artifacts.mjs makes for Linux
# (copyBuildFiles, prepareLinuxBinaries); nothing is uploaded from here. From
# chromium-158-buildkite's package-artifacts.sh.
set -euo pipefail

dest=${1:?package: no destination directory}
out=/chromium/src/out/Release

build_id=$(sed -n 's/^#define RECORD_REPLAY_BUILD_ID "\(.*\)"$/\1/p' /chromium/src/base/record_replay_driver.h)
if [ -z "$build_id" ]; then
  echo "package: no RECORD_REPLAY_BUILD_ID in base/record_replay_driver.h" >&2
  exit 1
fi

stage=$dest/replay-chromium
mkdir -p "$stage"
cd "$out"
# Without these there is no runtime to package.
cp -a chrome locales replay-assets "$stage/"
for file in icudtl.dat v8_context_snapshot.bin vk_swiftshader_icd.json replay \
            chrome_crashpad_handler *.pak *.pak.info *.so *.so.1; do
  if [ -e "$file" ]; then
    cp -a "$file" "$stage/"
  fi
done
ls -la "$stage"

# Parallel, with reasonable compression, as master does.
tar -C "$dest" -c -I "xz -2 -T0" --remove-files -f "$dest/$build_id.tar.xz" replay-chromium
printf '%s' "$build_id" > "$dest/build_id"
ls -la "$dest"
