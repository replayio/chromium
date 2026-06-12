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
spawnChecked(
  "git",
  ["config", "--global", "--add", "safe.directory", __dirname],
  {
    stdio: "inherit",
  }
);

if (currentPlatform() == "macOS") {
  // Make sure the main executable gets rebuilt with the new build ID.
  spawnChecked("touch", [`${__dirname}/chrome/app/chrome_exe_main_mac.cc`]);
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

fs.writeFileSync(
  `${__dirname}/base/record_replay_driver.cc`,
  `
namespace recordreplay {
  char gRecordReplayDriver[] = "${driverString}";
  int gRecordReplayDriverSize = ${driverContents.length};
  char gBuildId[] = "${buildId}";
}
`
);

fs.writeFileSync(
  `${__dirname}/base/record_replay_driver.h`,
  `
#ifndef BASE_RECORD_REPLAY_DRIVER_H_
#define BASE_RECORD_REPLAY_DRIVER_H_

#define RECORD_REPLAY_BUILD_ID "${buildId}"

#endif // BASE_RECORD_REPLAY_DRIVER_H_
`
);

// regenerate reclient/reproxy config so it targets the EngFlow RBE_* env
// vars that are set inside this build container. reclient_custom.py overrides
// the instance away from Chromium's corp-only 'rbe-chrome-untrusted'.
console.log(`[reclient] RBE_service=${process.env.RBE_service || "(unset)"}`);
console.log(`[reclient] RBE_instance=${process.env.RBE_instance || "(unset)"}`);
console.log(
  `[reclient] CHROMIUM_BUILDTOOLS_PATH=${process.env.CHROMIUM_BUILDTOOLS_PATH || "(unset)"}`
);
console.log(`[reclient] cwd=${process.cwd()} __dirname=${__dirname}`);

const autoninjaPath = spawnSync("which", ["autoninja"])
  .stdout.toString()
  .trim();
const depotToolsDir = autoninjaPath
  ? path.dirname(autoninjaPath)
  : "/depot_tools";
const depotToolsRevision = spawnSync("git", [
  "-C",
  depotToolsDir,
  "rev-parse",
  "HEAD",
])
  .stdout.toString()
  .trim();
console.log(`[reclient] autoninja=${autoninjaPath || "(not found)"}`);
console.log(
  `[reclient] depot_tools=${depotToolsDir} revision=${depotToolsRevision || "(unknown)"}`
);

// autoninja runs depot_tools' bundled python (>=3.9), not the container's
// system python3 (3.8). configure_reclient and our probe must use the same
// interpreter, otherwise they hit/skip different code paths.
const depotPython = path.join(depotToolsDir, "python-bin", "python3");
console.log(
  `[reclient] depotPython=${depotPython} exists=${fs.existsSync(depotPython)}`
);

// Resolve the cfg path EXACTLY as autoninja does, and report what
// autoninja's own logic resolves the RBE project to. This verifies, end to
// end, which file autoninja reads and which instance it derives from it.
function probeReclient(phase) {
  const py = [
    "import sys, os",
    `sys.path.insert(0, ${JSON.stringify(depotToolsDir)})`,
    "import reclient_helper, gclient_paths",
    "cfg = reclient_helper.find_reclient_cfg() or ''",
    "print('PHASE=' + " + JSON.stringify(phase) + ")",
    "print('buildtools_path=' + (gclient_paths.GetBuildtoolsPath() or '(none)'))",
    "print('primary_solution=' + (gclient_paths.GetPrimarySolutionPath() or '(none)'))",
    "print('find_reclient_cfg=' + (cfg or '(none)'))",
    "print('exists=' + str(bool(cfg and os.path.isfile(cfg))))",
    "lines = open(cfg).read().splitlines() if cfg and os.path.isfile(cfg) else []",
    "inst = [l for l in lines if l.strip().startswith('instance')]",
    "print('instance_lines=' + repr(inst))",
  ].join("; ");
  const out = spawnSync(depotPython, ["-c", py], { cwd: process.cwd() });
  process.stdout.write(
    "[reclient] " +
      out.stdout.toString().trim().split("\n").join("\n[reclient] ") +
      "\n"
  );
  const err = out.stderr.toString().trim();
  if (err) {
    console.log(`[reclient] probe stderr: ${err}`);
  }
}

const reproxyCfgPath = path.join(
  __dirname,
  "buildtools",
  "reclient_cfgs",
  "reproxy.cfg"
);
console.log(`[reclient] expected configure_reclient output=${reproxyCfgPath}`);

spawnChecked(
  depotPython,
  [
    "third_party/reclient_configs/configure_reclient.py",
    "--src_dir=.",
    `--custom_py=${path.join(__dirname, "replay_build_scripts", "reclient_custom.py")}`,
  ],
  { stdio: "inherit" }
);

probeReclient("after configure_reclient");

// ensure that build configuration is written with correct paths
const gn = currentPlatform() == "windows" ? "gn.bat" : "gn";
spawnChecked(gn, ["gen", outdir], { stdio: "inherit" });

probeReclient("after gn gen");

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

probeReclient("before autoninja");

console.log(`Building...`);
const autoninja =
  currentPlatform() == "windows" ? "autoninja.bat" : "autoninja";
// make the windows build verbose so we can see what's going on
const platformAutoNinjaArgs = currentPlatform() == "windows" ? ["-v"] : [];
spawnChecked(autoninja, [...platformAutoNinjaArgs, "-C", outdir, "chrome"], {
  stdio: "inherit",
});

console.log(`Build finished.`);

function spawnChecked(cmd, args, options) {
  const prettyCmd = [cmd].concat(args).join(" ");
  console.error("$ " + prettyCmd);

  const rv = spawnSync(cmd, args, options);

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
