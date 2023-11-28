// Script used by buildkite to build Chromium for Windows in CI
import fs from "fs";
import { spawnChecked } from "./replay_build_scripts/common.mjs";

// make the current working directory the realpath of the current working directory
// this is necessary on Windows if you are building chromium from outside of the
// directory using a symlink
process.chdir(fs.realpathSync(process.cwd()));

// TODO(dmiller): remove this hack when we switch to the new ci system
spawnChecked("git", ["apply", "replay_build_scripts/windows.patch"]);

try {
  spawnChecked("node", ["build.js"], { stdio: "inherit" });
} finally {
  spawnChecked("git", [
    "checkout",
    "media/audio/win/audio_low_latency_input_win.cc",
  ]);
}
