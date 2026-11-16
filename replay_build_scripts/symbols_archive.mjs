// The symbols archive of a Linux build that is never uploaded to S3 (Chromium 157
// CI, replay_build_scripts/ci/job.sh): the one upload_build_artifacts.mjs makes
// and uploads to symbols/, written next to the packaged build instead. A test
// step hands its directory to the backend as REPLAY_SYMBOLS_DIR.
//
//   node replay_build_scripts/symbols_archive.mjs <dest dir>
//
// Run from the src directory. <dest dir> holds the `build_id` file
// (ci/container/package.sh); <dest dir>/<build id>.symbols.tgz is written, for
// out/Release/chrome.
import fs from "fs";
import path from "path";

import { buildSymbolsArchive } from "./symbolication.mjs";

const dest = process.argv[2];
if (!dest) {
  throw new Error("usage: symbols_archive.mjs <dest dir>");
}
const buildId = fs.readFileSync(path.join(dest, "build_id"), "utf8").trim();
if (!buildId) {
  throw new Error(`no build id in ${path.join(dest, "build_id")}`);
}
const objectDirectory = path.resolve("out", "Release");

process.chdir(dest);
const archive = await buildSymbolsArchive(
  buildId,
  objectDirectory,
  ["chrome"],
  /* useArm */ false
);
console.log(`symbols: ${path.join(dest, archive)}`);
