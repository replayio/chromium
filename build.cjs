// Copyright 2021 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Build the Replay Chromium runtime: embed the record/replay driver, configure
// the out dir and build `chrome`.
//
// master's build.js for Chromium 157: siso with remote execution instead of
// ninja with reclient. .cjs because //package.json makes .js an ES module.
// The CI image's node is 12: no optional chaining, fs.rmSync or fs.cpSync.

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const options = {
  // Also the cue that this is a developer's machine.
  localBuildExtension: process.env.LOCAL_DEVELOPER_BUILD_EXTENSION || "",
  localDriverDir: process.env.REPLAY_LOCAL_DRIVER_DIR,
  driverRevision: process.env.DRIVER_REVISION,
  buildArm: !!process.env.REPLAY_BUILD_ARM,
  inBuildkite: !!process.env.BUILDKITE,
  branch: process.env.BUILDKITE_BRANCH,
  defaultBranch: process.env.BUILDKITE_PIPELINE_DEFAULT_BRANCH,
  // Failed steps before the build stops: 1 stops at the first, 0 never stops.
  maxFailures: countFromEnv("REPLAY_BUILD_MAX_FAILURES"),
  // Parallel local steps; siso's default is the CPU count.
  sisoLocalJobs: countFromEnv("REPLAY_SISO_LOCAL_JOBS"),
  // Parallel remote actions, and whether locally built results are uploaded.
  // Both outside CI only, both off unless asked for; see buildToolArgs.
  sisoRemoteJobs: countFromEnv("REPLAY_SISO_REMOTE_JOBS"),
  sisoCacheUpload: !!process.env.REPLAY_SISO_CACHE_UPLOAD,
  // siso `-config` names, e.g. to run more step kinds remotely.
  sisoConfig: process.env.REPLAY_SISO_CONFIG,
};

const outdir = options.buildArm ? "out/Release-ARM" : "out/Release";
const archSuffix = options.buildArm ? "-arm" : "";
const sisoDir = path.join(__dirname, "build", "config", "siso");
// Relative to the out dir. siso appends to it.
const SISO_FAILURE_SUMMARY = "siso_failure_summary";

main();

function main() {
  if (options.localDriverDir && options.driverRevision) {
    // local driver should generally be latest
    throw new Error(
      "Conflicting build settings: environment variables DRIVER_REVISION and REPLAY_LOCAL_DRIVER_DIR cannot coexist."
    );
  }
  trustRepository();
  embedDriver();
  checkBuildImage();
  checkRemoteExecution();
  prepareOutDir();
  spawnChecked(tool("gn"), ["gen", outdir], { stdio: "inherit" });
  lintOutsideCI();
  build();
  console.log(`Build finished.`);
}

// Otherwise: fatal: unsafe repository ('/chromium/src' is owned by someone
// else). The build image has a --system safe.directory already.
function trustRepository() {
  if (buildImageOfContainer()) {
    return;
  }
  spawnChecked(
    "git",
    ["config", "--global", "--add", "safe.directory", __dirname],
    { stdio: "inherit" }
  );
}

function embedDriver() {
  const driver = fetchDriver();
  console.log(
    `Embedding ${options.localDriverDir ? "LOCAL" : "DOWNLOADED"} driver from ${
      driver.file
    }...`
  );
  const contents = fs.readFileSync(driver.file);
  const { revision, date } = JSON.parse(fs.readFileSync(driver.json, "utf8"));
  if (!options.localDriverDir) {
    fs.unlinkSync(driver.file);
    fs.unlinkSync(driver.json);
  }

  const octal = [];
  for (let i = 0; i < contents.length; i++) {
    octal.push(`\\${contents[i].toString(8)}`);
  }
  const buildId = `${computeBuildId(date, revision)}${buildSuffix()}`;

  writeFileSyncIfChanged(
    `${__dirname}/base/record_replay_driver.cc`,
    `
namespace recordreplay {
  char gRecordReplayDriver[] = "${octal.join("")}";
  int gRecordReplayDriverSize = ${contents.length};
  char gBuildId[] = "${buildId}";
}
`
  );

  const headerChanged = writeFileSyncIfChanged(
    `${__dirname}/base/record_replay_driver.h`,
    `
#ifndef BASE_RECORD_REPLAY_DRIVER_H_
#define BASE_RECORD_REPLAY_DRIVER_H_

#define RECORD_REPLAY_BUILD_ID "${buildId}"

#endif // BASE_RECORD_REPLAY_DRIVER_H_
`
  );

  if (currentPlatform() == "macOS" && headerChanged) {
    // Make sure the main executable gets rebuilt with the new build ID.
    spawnChecked("touch", [`${__dirname}/chrome/app/chrome_exe_main_mac.cc`]);
  }
}

// The driver and its metadata: from REPLAY_LOCAL_DRIVER_DIR, or downloaded --
// DRIVER_REVISION's archive if that is set, the latest otherwise.
function fetchDriver() {
  const base = `${currentPlatform()}-recordreplay${archSuffix}`;
  const names = { file: `${base}.${driverExtension()}`, json: `${base}.json` };
  if (options.localDriverDir) {
    return {
      file: path.resolve(options.localDriverDir, names.file),
      json: path.resolve(options.localDriverDir, names.json),
    };
  }

  console.log(`Downloading driver...`);
  const archive = `${base}.tgz`;
  const remoteArchive = options.driverRevision
    ? `${currentPlatform()}-recordreplay-${options.driverRevision
        .trim()
        .substring(0, 12)}${archSuffix}.tgz`
    : archive;
  spawnChecked(
    "curl",
    ["-f", `https://static.replay.io/downloads/${remoteArchive}`, "-o", archive],
    { stdio: "inherit" }
  );
  spawnChecked("tar", ["xf", archive], { stdio: "inherit" });
  fs.unlinkSync(archive);
  return names;
}

function buildSuffix() {
  return options.branch !== options.defaultBranch
    ? "-dev"
    : options.localBuildExtension;
}

// In the build image (replay_build_scripts/ci/), refuse to build a checkout
// whose Dockerfile is not the one this image was built from: its tag is a
// hash of that file.
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
  return crypto
    .createHash("sha256")
    .update(fs.readFileSync(dockerfile))
    .digest("hex")
    .slice(0, 16);
}

// In the build image, compiles must go to the REAPI backend that gclient's
// configure_siso hook set up from .gclient (replay_build_scripts/ci/gclient),
// with the mTLS client pair siso reads from RBE_tls_client_auth_*. Refuse
// anything else before a step runs: the hook's default is Google's RBE, and
// the agent cannot compile Chromium itself.
function checkRemoteExecution() {
  if (!buildImageOfContainer()) {
    // Two knobs for a developer build, both off unless the environment asks.
    //
    // REPLAY_SISO_REMOTE_JOBS caps concurrent remote actions. siso's own
    // default is 80 per CPU, which is more than this cluster runs at once: the
    // surplus queues until a step's two-minute timeout expires and then
    // compiles locally. A cap stops that waiting, but it also stops the local
    // cores taking the overflow, because siso keeps one fast-local slot
    // (SISO_LIMITS fastlocal=), so whether a cap is faster depends on the size
    // of the burst and is not settled. 0 means siso's default. Needs a backend:
    // autoninja runs `siso ninja --offline` when use_remoteexec is false, and
    // siso then refuses a positive -remote_jobs.
    if (options.sisoRemoteJobs) {
      args.push(`-remote_jobs=${options.sisoRemoteJobs}`);
    }
    // REPLAY_SISO_CACHE_UPLOAD uploads the results of locally executed steps,
    // so the next build of the same sources reads them back. CI reads the same
    // cache instance, a local step runs unsandboxed, and a step's key covers
    // only the inputs siso declared, so this stays opt-in until whoever owns
    // that cache signs it off.
    if (options.sisoCacheUpload) {
      args.push("-re_cache_enable_write");
    }
  }
  if (options.sisoLocalJobs !== undefined) {
    args.push(`-local_jobs=${options.sisoLocalJobs}`);
  }
  if (options.sisoConfig) {
    args.push("-config", options.sisoConfig);
  }
  return args;
}

function countFromEnv(name) {
  const value = process.env[name];
  if (value === undefined || value === "") {
    return undefined;
  }
  if (!/^[0-9]+$/.test(value)) {
    throw new Error(`${name}="${value}" is not a count`);
  }
  return +value;
}

function tool(name) {
  return currentPlatform() == "windows" ? `${name}.bat` : name;
}

function readFileOrNull(file) {
  try {
    return fs.readFileSync(file, "utf8");
  } catch (e) {
    if (e.code == "ENOENT") {
      return null;
    }
    throw e;
  }
}

function writeFileSyncIfChanged(filename, newContents) {
  if (readFileOrNull(filename) === newContents) {
    console.log(`Skipping ${filename} because it hasn't changed.`);
    return false;
  }
  fs.writeFileSync(filename, newContents);
  return true;
}

function spawnChecked(cmd, args, spawnOptions) {
  const prettyCmd = [cmd].concat(args).join(" ");
  console.error("$ " + prettyCmd);

  const rv = spawnSync(cmd, args, spawnOptions);

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
  if (options.localBuildExtension) {
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
