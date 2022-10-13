// Copyright 2021 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

// If this env var is set, we then we (also) use this as our cue that
// we're building on a developer's machine, and will run some different
// logic.
const IS_LOCAL_BUILD = !!process.env["LOCAL_DEVELOPER_BUILD_EXTENSION"];

const { REPLAY_LOCAL_DRIVER_DIR, DRIVER_REVISION } = process.env;
const driverRevisionIsSet = !!DRIVER_REVISION;

if (REPLAY_LOCAL_DRIVER_DIR && driverRevisionIsSet) {
  // local driver should generally be latest
  throw new Error(
    "Conflicting build settings: environment variables DRIVER_REVISION and REPLAY_LOCAL_DRIVER_DIR cannot coexist."
  );
}

const buildArm = !!process.env.REPLAY_BUILD_ARM;
const outdir = buildArm ? "out/Release-ARM" : "out/Release";

// Ensure that the git repository is "trusted", otherwise we'll get errors like:
// fatal: unsafe repository ('/chromium/src' is owned by someone else)
// The build image already has a --system safe.directory for everything, and
// its HOME is a directory that persists across builds, so adding a --global
// line there on every build would just make the file longer.
if (!fs.existsSync("/etc/chromium-build-image")) {
  spawnChecked(
    "git",
    ["config", "--global", "--add", "safe.directory", __dirname],
    {
      stdio: "inherit",
    }
  );
}

const archSuffix = buildArm ? "-arm" : "";

if (!REPLAY_LOCAL_DRIVER_DIR) {
  // Download the record/replay driver archive. If DRIVER_REVISION is set,
  // download that specific version. Otherwise download the latest.
  console.log(`Downloading driver...`);
  const driverArchive = `${currentPlatform()}-recordreplay${archSuffix}.tgz`;

  if (driverRevisionIsSet) {
    const downloadArchive = `${currentPlatform()}-recordreplay-${DRIVER_REVISION.trim().substring(0,12)}${archSuffix}.tgz`;
    const downloadUrl = `https://static.replay.io/downloads/${downloadArchive}`;
    spawnChecked(
      "curl",
      ["-f", downloadUrl, "-o", driverArchive],
      { stdio: "inherit" }
    );
  } else {
    const downloadUrl = `https://static.replay.io/downloads/${driverArchive}`;
    console.log(`Downloading latest driver: ${downloadUrl}`);
    spawnChecked(
      "curl",
      ["-f", downloadUrl, "-o", driverArchive],
      { stdio: "inherit" }
    );
  }
  spawnChecked("tar", ["xf", driverArchive], { stdio: "inherit" });
  fs.unlinkSync(driverArchive);
}

let driverFile = `${currentPlatform()}-recordreplay${archSuffix}.${driverExtension()}`;
let driverJSON = `${currentPlatform()}-recordreplay${archSuffix}.json`;
if (REPLAY_LOCAL_DRIVER_DIR) {
  driverFile = path.resolve(REPLAY_LOCAL_DRIVER_DIR, driverFile);
  driverJSON = path.resolve(REPLAY_LOCAL_DRIVER_DIR, driverJSON);
}

// Embed the driver in the source.
console.log(
  `Embedding ${
    REPLAY_LOCAL_DRIVER_DIR ? "LOCAL" : "DOWNLOADED"
  } driver from ${driverFile}...`
);
const driverContents = fs.readFileSync(driverFile);
const { revision: driverRevision, date: driverDate } = JSON.parse(
  fs.readFileSync(driverJSON, "utf8")
);

if (!REPLAY_LOCAL_DRIVER_DIR) {
  // cleanup
  fs.unlinkSync(driverFile);
  fs.unlinkSync(driverJSON);
}

let driverString = [];
for (let i = 0; i < driverContents.length; i++) {
  driverString.push(`\\${driverContents[i].toString(8)}`);
}
driverString = driverString.join("");

const buildSuffix =
  process.env["BUILDKITE_BRANCH"] !==
  process.env["BUILDKITE_PIPELINE_DEFAULT_BRANCH"]
    ? "-dev"
    : process.env["LOCAL_DEVELOPER_BUILD_EXTENSION"] || "";
const buildId = `${computeBuildId(driverDate, driverRevision)}${buildSuffix}`;

writeFileSyncIfChanged(
  `${__dirname}/base/record_replay_driver.cc`,
  `
namespace recordreplay {
  char gRecordReplayDriver[] = "${driverString}";
  int gRecordReplayDriverSize = ${driverContents.length};
  char gBuildId[] = "${buildId}";
}
`
);

const driver_h_changed = writeFileSyncIfChanged(
  `${__dirname}/base/record_replay_driver.h`,
  `
#ifndef BASE_RECORD_REPLAY_DRIVER_H_
#define BASE_RECORD_REPLAY_DRIVER_H_

#define RECORD_REPLAY_BUILD_ID "${buildId}"

#endif // BASE_RECORD_REPLAY_DRIVER_H_
`
);

if (currentPlatform() == "macOS" && driver_h_changed) {
  // Make sure the main executable gets rebuilt with the new build ID.
  spawnChecked("touch", [`${__dirname}/chrome/app/chrome_exe_main_mac.cc`]);
}

// ensure that build configuration is written with correct paths
const gn = currentPlatform() == "windows" ? "gn.bat" : "gn";
checkBuildImage();
configureReclient();
prepareOutDir();
spawnChecked(gn, ["gen", outdir], { stdio: "inherit" });

// only lint when not in buildkite (since buildkite does the linting at a different stage)
if (!process.env["BUILDKITE"]) {
  console.log(`Linting replay js blobs...`);
  let cwd;
  try {
    cwd = process.cwd();
    process.chdir(path.join(__dirname, "replay_build_scripts"));
    spawnChecked("npm", ["ci"], { stdio: "inherit" });
  } finally {
    process.chdir(cwd);
  }
  spawnChecked("node", [path.join("replay_build_scripts", "lint.mjs")], {
    stdio: "inherit",
  });
}

console.log(`Building...`);
const autoninja =
  currentPlatform() == "windows" ? "autoninja.bat" : "autoninja";
// make the windows build verbose so we can see what's going on
const platformAutoNinjaArgs = currentPlatform() == "windows" ? ["-v"] : [];
const autoninjaArgs = [...platformAutoNinjaArgs, "-C", outdir, "chrome"];
if (process.env["BUILDKITE"]) {
  buildHealingNinjaState();
} else {
  spawnChecked(autoninja, autoninjaArgs, { stdio: "inherit" });
}

console.log(`Build finished.`);

// CI agents build in a persistent checkout, where an interrupted or
// overlapping ninja run can leave state behind in the out dir that makes every
// later ninja crash at startup. Clean the out dir and build again if ninja
// crashes; RBE's cache keeps the rebuild cheap. gn clean keeps args.gn (which
// prepareOutDir would write again anyway).
function buildHealingNinjaState() {
  const rv = spawnLogged(autoninja, autoninjaArgs, { stdio: "inherit" });
  if (!ninjaCrashed(rv)) {
    checkSpawnResult(rv);
    return;
  }
  console.error(
    `ninja crashed (exit code ${rv.status}), cleaning ${outdir} and retrying`
  );
  spawnChecked(gn, ["clean", outdir], { stdio: "inherit" });
  spawnChecked(gn, ["gen", outdir], { stdio: "inherit" });
  spawnChecked(autoninja, autoninjaArgs, { stdio: "inherit" });
}

// autoninja.py exits with sys.exit(-signal) when ninja dies from a signal,
// i.e. 256 - signal: 245 for SIGSEGV. A failed build step exits with 1.
function ninjaCrashed(rv) {
  const crashSignals = [6 /* SIGABRT */, 7 /* SIGBUS */, 11 /* SIGSEGV */];
  return crashSignals.some((signal) => rv.status == 256 - signal);
}

// The reclient configs, from the tree, for the path the build actually runs
// at. DEPS pins the generator (third_party/reclient_configs) and the generated
// rewrapper config is what names the remote worker image by digest, so the
// worker image is part of the checkout like the rest of the toolchain -- but the
// generated reproxy config also holds an absolute path to the deps scanner in
// the checkout, so a config generated anywhere else points at a path that does
// not exist here and deps scanning silently falls back to compiling locally.
//
// gclient's configure_reclient hook, which would do this, never runs on CI (see
// runGclientSync in replay_build_scripts/common.mjs), so until now these files
// were whatever the last person to sync the agent's checkout by hand produced.
// Only in our own build image, where the checkout is always at /chromium/src:
// on a developer's machine the files belong to however they sync their tree.
function configureReclient() {
  if (!buildImageOfContainer() || !process.env["RBE_service"]) {
    return;
  }
  const parent = path.dirname(__dirname);
  const configure = path.join(
    __dirname,
    "third_party",
    "reclient_configs",
    "configure_reclient.py"
  );
  if (!fs.existsSync(configure)) {
    throw new Error(
      `${configure} is missing: this checkout's DEPS have not been synced ` +
        `(replay_build_scripts/ci/checkout.sh prepare)`
    );
  }
  console.log("Configuring reclient...");
  spawnChecked("python3", [configure, "--src_dir=" + path.basename(__dirname)], {
    cwd: parent,
    stdio: "inherit",
  });
}

// In the build image (replay_build_scripts/ci/), refuse to build a checkout
// whose Dockerfile is not the one this image was built from: its tag is a hash
// of that file, so a tree that changed it -- another Chromium version's, say --
// would otherwise build with another version's tools and say nothing.
function checkBuildImage() {
  const have = buildImageOfContainer();
  if (!have) {
    return;
  }
  const want = buildImageOfCheckout();
  if (have != want) {
    throw new Error(
      `This container is build image ${have}, but this checkout's ` +
        `replay_build_scripts/ci/Dockerfile is ${want}. Build it with ` +
        `replay_build_scripts/ci/build-in-container.sh, which builds the image ` +
        `this checkout describes.`
    );
  }
}

function buildImageOfContainer() {
  try {
    return fs.readFileSync("/etc/chromium-build-image", "utf8").trim();
  } catch (e) {
    return "";
  }
}

function buildImageOfCheckout() {
  const dockerfile = path.join(
    __dirname,
    "replay_build_scripts",
    "ci",
    "Dockerfile"
  );
  // build-image.sh: sha256 of the Dockerfile, first 16 characters.
  return require("crypto")
    .createHash("sha256")
    .update(fs.readFileSync(dockerfile))
    .digest("hex")
    .slice(0, 16);
}

// The out dir's own configuration comes from the repo.
//
// args.gn used to be written by hand on each machine, which made it the one
// piece of the build configuration that no checkout described: when a build
// deleted the out dir to recover from crashing ninja state, gn gen then
// produced a debug build without remote execution, and the recovery had to be
// changed to preserve the file instead (replayio/chromium#1544). A wiped out
// dir now comes back correctly configured.
//
// The build agent's args.gn is not an import but a verbatim copy of
// replay_build_scripts/linux_args.gn, which means the file in this repository
// is decorative there: changing it changes nothing a build does. A copy that
// still matches the repository's is replaced with the import, which resolves to
// exactly the same arguments and makes the repository the source of them from
// then on. An args.gn that says anything else is somebody's own and is left
// alone.
function prepareOutDir() {
  mkdirp(outdir);
  const argsFile = path.join(outdir, "args.gn");
  const args = `replay_build_scripts/${argsName()}`;
  const line = `import("//${args}")\n`;
  if (!fs.existsSync(argsFile)) {
    console.log(`Writing ${argsFile} importing //${args}`);
    fs.writeFileSync(argsFile, line);
  } else if (isCopyOf(argsFile, path.join(__dirname, args))) {
    console.log(`Replacing the copy in ${argsFile} with //${args}`);
    fs.writeFileSync(argsFile, line);
  }
}

function isCopyOf(file, original) {
  function read(f) {
    try {
      return fs.readFileSync(f, "utf8").replace(/\s+$/, "");
    } catch (e) {
      return null;
    }
  }
  const have = read(file);
  return have !== null && have === read(original);
}

// The build image's node is 8.10 (bionic), so: no fs.mkdirSync recursive
// option, no fs.rmSync, no fs.cpSync. Everything build.js does runs there.
function mkdirp(dir) {
  const parent = path.dirname(dir);
  if (parent != dir && !fs.existsSync(parent)) {
    mkdirp(parent);
  }
  if (!fs.existsSync(dir)) {
    fs.mkdirSync(dir);
  }
}

function argsName() {
  switch (currentPlatform()) {
    case "linux":
      return "linux_args.gn";
    case "windows":
      return "windows_args.gn";
    case "macOS":
      return buildArm ? "mac_arm64_args.gn" : "mac_x86_64_args.gn";
  }
}

function writeFileSyncIfChanged(filename, newContents) {
  let changed = false;
  try {
    const oldContents = fs.readFileSync(filename, "utf8");
    changed = oldContents != newContents;
  } catch (e) {
    changed = true;
  }

  if (!changed) {
    console.log(`Skipping ${filename} because it hasn't changed.`);
  } else {
    fs.writeFileSync(filename, newContents);
  }

  return changed;
}

function spawnChecked(cmd, args, options) {
  return checkSpawnResult(spawnLogged(cmd, args, options));
}

function spawnLogged(cmd, args, options) {
  const prettyCmd = [cmd].concat(args).join(" ");
  console.error("$ " + prettyCmd);

  return spawnSync(cmd, args, options);
}

function checkSpawnResult(rv) {
  if (rv.status != 0 || rv.error) {
    console.error(
      `Process failed: err=${rv.error || ""}, signal=${rv.signal || ""}`
    );
    throw new Error(`Spawned process failed with exit code ${rv.status}`);
  }

  return rv;
}

function currentPlatform() {
  switch (process.platform) {
    case "darwin":
      return "macOS";
    case "linux":
      return "linux";
    case "win32":
      return "windows";
    default:
      throw new Error(`Platform ${process.platform} not supported`);
  }
}

function driverExtension() {
  return currentPlatform() == "windows" ? "dll" : "so";
}

/**
 * @returns {string} "YYYYMMDD" format of UTC timestamp of given revision.
 */
function getRevisionDate(revision = "HEAD", spawnOptions = undefined) {
  const dateString = spawnChecked(
    "git",
    ["show", revision, "--pretty=%cd", "--date=iso-strict", "--no-patch"],
    spawnOptions
  )
    .stdout.toString()
    .trim();

  // convert to UTC -> then get the date only
  // explanations: https://github.com/replayio/backend/pull/7115#issue-1587869475
  return new Date(dateString).toISOString().substring(0, 10).replace(/-/g, "");
}

/**
 * WARNING: We have copy-and-pasted `computeBuildId` into all our runtimes and `backend`.
 * When changing this: always keep all versions of this in sync, or else, builds will break.
 */
function computeBuildId(driverDate, driverRevision) {
  let chromiumRevision = "";
  if (IS_LOCAL_BUILD) {
    // For local builds, we just generate a random hash, in order to ensure s3 freshness.
    const LOOKUP = "0123456789abcdef";
    for (let i = 0; i < 12; i++) {
      chromiumRevision += LOOKUP[Math.floor(Math.random() * 16)];
    }
  } else {
    // Note: this build ID doesn't include revision etc. information for v8 or other inner git
    // repositories. It would be good to either fix this or enforce that the chromium revision
    // gets bumped whenever an inner repository changes.
    chromiumRevision = spawnChecked("git", ["rev-parse", "--short=12", "HEAD"])
      .stdout.toString()
      .trim();
  }

  const runtimeDate = getRevisionDate();

  // Use the later of the two dates in the build ID.
  const date = +runtimeDate >= +driverDate ? runtimeDate : driverDate;

  return `${currentPlatform()}-chromium-${date}-${chromiumRevision}-${driverRevision}`;
}
