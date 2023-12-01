// Script used by buildkite to build Chromium for macOS in CI
import fs from "fs";
import { spawnChecked } from "./replay_build_scripts/common.mjs";

spawnChecked("node", ["build.js"], { stdio: "inherit" });
fs.rmSync(path.join(outdir, "Replay-Chromium.app"), {
  recursive: true,
  force: true,
});
fs.renameSync("out/Release/Chromium.app", "out/Release/Replay-Chromium.app");
