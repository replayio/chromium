// Script used by buildkite to build Chromium for macOS in CI
import fs from "fs";
import path from "path";
import { spawnChecked } from "./replay_build_scripts/common.mjs";

const buildArm = !!process.env.REPLAY_BUILD_ARM;
const outdir = buildArm ? "out/Release-ARM" : "out/Release";

spawnChecked("node", ["build.js"], { stdio: "inherit" });
fs.rmSync(path.join(outdir, "Replay-Chromium.app"), {
  recursive: true,
  force: true,
});
fs.renameSync("out/Release/Chromium.app", "out/Release/Replay-Chromium.app");
