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
  // Parallel remote actions outside CI; see buildToolArgs.
  sisoRemoteJobs: countFromEnv("REPLAY_SISO_REMOTE_JOBS"),
  // siso `-config` names, e.g. to run more step kinds remotely.
  sisoConfig: process.env.REPLAY_SISO_CONFIG,
};

const outdir = options.buildArm ? "out/Release-ARM" : "out/Release";
const archSuffix = options.buildArm ? "-arm" : "";
const sisoDir = path.join(__dirname, "build", "config", "siso");
// Relative to the out dir. siso appends to it.
const SISO_FAILURE_SUMMARY = "siso_failure_summary";
// Concurrent remote actions a developer's build asks for; see buildToolArgs.
const DEVELOPER_REMOTE_JOBS = 50;

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
    return;
  }
  const sisoEnv = readSisoEnv();
  if (sisoEnv.SISO_PROJECT || !sisoEnv.SISO_REAPI_ADDRESS) {
    throw new Error(
      `build/config/siso/.sisoenv does not name a non-Google REAPI backend ` +
        `(${JSON.stringify(sisoEnv)}): .gclient custom_vars are missing`
    );
  }
  const backendDir = path.join(sisoDir, "backend_config");
  const installed = readFileOrNull(path.join(backendDir, "backend.star"));
  const ours = fs.readFileSync(path.join(backendDir, "replay_engflow.star"), "utf8");
  if (installed !== ours) {
    throw new Error(
      `build/config/siso/backend_config/backend.star is not replay_engflow.star: ` +
        `gclient runhooks has not run with this checkout's .gclient`
    );
  }
  for (const name of ["RBE_tls_client_auth_cert", "RBE_tls_client_auth_key"]) {
    const file = process.env[name];
    if (!file || !fs.existsSync(file)) {
      throw new Error(`${name} does not name a file: no EngFlow client pair`);
    }
  }
  console.log(
    `Remote execution: ${sisoEnv.SISO_REAPI_ADDRESS} instance ${sisoEnv.SISO_REAPI_INSTANCE}`
  );
}

// KEY=VALUE lines, written by build/config/siso/configure_siso.py.
function readSisoEnv() {
  const text = readFileOrNull(path.join(sisoDir, ".sisoenv"));
  if (text === null) {
    throw new Error(
      `build/config/siso/.sisoenv is missing: gclient runhooks has not run`
    );
  }
  const entries = {};
  for (const line of text.split("\n")) {
    const eq = line.indexOf("=");
    if (eq > 0) {
      entries[line.slice(0, eq).trim()] = line.slice(eq + 1).trim();
    }
  }
  return entries;
}

// The out dir's configuration comes from the repo. In the build image the
// out dir is CI's own, so args.gn is always the import; elsewhere an existing
// args.gn is somebody's own and is left alone.
function prepareOutDir() {
  fs.mkdirSync(outdir, { recursive: true });
  const argsFile = path.join(outdir, "args.gn");
  if (buildImageOfContainer() || !fs.existsSync(argsFile)) {
    writeFileSyncIfChanged(
      argsFile,
      `import("//replay_build_scripts/${argsName()}")\n`
    );
  }
}

function argsName() {
  switch (currentPlatform()) {
    case "linux":
      return "linux_args.gn";
    case "windows":
      return "windows_args.gn";
    case "macOS":
      return options.buildArm ? "mac_arm64_args.gn" : "mac_x86_64_args.gn";
  }
}

// only lint when not in buildkite (since buildkite does the linting at a different stage)
function lintOutsideCI() {
  if (options.inBuildkite) {
    return;
  }
  console.log(`Linting replay js blobs...`);
  spawnChecked("npm", ["ci"], {
    cwd: path.join(__dirname, "replay_build_scripts"),
    stdio: "inherit",
  });
  spawnChecked("node", [path.join("replay_build_scripts", "lint.mjs")], {
    stdio: "inherit",
  });
}

function build() {
  console.log(`Building...`);
  const summary = path.join(outdir, SISO_FAILURE_SUMMARY);
  if (fs.existsSync(summary)) {
    fs.unlinkSync(summary);
  }
  // make the windows build verbose so we can see what's going on
  const platformArgs = currentPlatform() == "windows" ? ["-v"] : [];
  spawnChecked(
    tool("autoninja"),
    [...platformArgs, "-C", outdir, ...buildToolArgs(), "chrome"],
    { stdio: "inherit" }
  );
}

// autoninja passes these on to `siso ninja` (use_siso in args.gn).
function buildToolArgs() {
  const args = [];
  if (options.maxFailures !== undefined) {
    args.push("-k", String(options.maxFailures));
  }
  if (buildImageOfContainer()) {
    // --strict_remote: a remote step never falls back to the agent's CPUs,
    // and no REAPI backend at all is an error.
    args.push("--strict_remote=true", "-failure_summary", SISO_FAILURE_SUMMARY);
  }
  if (!buildImageOfContainer()) {
    // siso asks for 80 concurrent remote actions per CPU, which is 2,880 on a
    // 36-core workstation and far more than this cluster runs at once. The
    // surplus sits queued until each step's two-minute timeout expires, and
    // siso then compiles it locally, having waited for nothing. Measured on a
    // burst of 218 dirty v8 TUs: 50 jobs, no timeouts, 88 s; 200 jobs, 45
    // timeouts, 171 s; siso's default, 8,068 timeouts inside a 45-minute
    // `chrome` build whose actual work was 8,076 compiles.
    //
    // CI keeps siso's default: it runs with --strict_remote, where a timeout
    // fails the build instead of hiding in the wall clock, and none of this was
    // measured on an agent.
    args.push(`-remote_jobs=${options.sisoRemoteJobs || DEVELOPER_REMOTE_JOBS}`);
    // Upload what this build compiles locally, so the next build of the same
    // sources -- another tree, another session, a rebuild after a branch switch
    // -- reads it from the cache. Measured: four TUs compiled in 20 s, then
    // served from the cache in 3 s. Only locally executed steps are uploaded,
    // and a step's digest covers the tree's own hermetic toolchain, so a result
    // can only ever be read back for the same action.
    args.push("-re_cache_enable_write");
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
