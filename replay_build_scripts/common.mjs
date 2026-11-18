import { spawnSync } from "child_process";
import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";
import { dirname } from "path";

// NOTE(dmiller): see https://stackoverflow.com/a/62892482 for explanation
const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

export function spawnChecked(cmd, args, options) {
  const prettyCmd = [cmd].concat(args).join(" ");
  log("$" + prettyCmd);

  const rv = spawnSync(cmd, args, options);

  if (rv.status != 0 || rv.error) {
    console.group(`Spawn FAILED (${rv.error || ""}) - All output:\n`);
    const stdout = rv.stdout ? rv.stdout.toString() : "";
    const stderr = rv.stderr ? rv.stderr.toString() : "";
    const allOutput = `${stdout} ${stderr}`.trim();
    if (allOutput) {
      console.error(allOutput);
    }
    console.groupEnd();
    throw new Error(`Spawned process failed with exit code ${rv.status}`);
  }

  return rv;
}

export const Platform = {
  macOS: "macOS",
  linux: "linux",
  windows: "windows",
};

// Get the ID for the current platform we are running on.
export function currentPlatform() {
  switch (process.platform) {
    case "darwin":
      return Platform.macOS;
    case "linux":
      return Platform.linux;
    case "win32":
      return Platform.windows;
    default:
      throw new Error(`Platform ${process.platform} not supported`);
  }
}

export function outputArchitecture() {
  switch (currentPlatform()) {
    case Platform.macOS:
      if (process.env.REPLAY_BUILD_ARM) {
        return "arm64";
      }
      return "x86_64";
    case Platform.linux:
      return "x86_64";
    case Platform.windows:
      return "x86_64";
  }
}

export function assert(v, why = "") {
  if (!v) {
    const error = new Error(`Assertion Failed: ${why}`);
    error.name = "AssertionFailure";
    throw error;
  }
}

export function log(s) {
  console.log(s);
}

export function toNumber(str) {
  const rv = +str;
  if (Number.isNaN(rv) && str != "(nil)") {
    throw new Error(`toNumber failed: "${str}"`);
  }
  return rv;
}

function maybeDeleteGitLockFile(dir) {
  const lockFile = path.join(dir, ".git", "index.lock");
  if (fs.existsSync(lockFile)) {
    fs.unlinkSync(lockFile);
  }
}

export function gn() {
  return currentPlatform() == Platform.windows ? "gn.bat" : "gn";
}

function runGnGen() {
  spawnChecked(gn(), ["gen", "out/Release"], { stdio: "inherit" });
}

// Puts `dir` at `revision` of `url`, fetching only that revision and only if
// `dir` lacks it. A checkout's origin may be gclient's cache mirror, which
// lacks new commits, and fetching every ref into a shallow checkout fetches
// the history of all of them.
export function syncRepo(dir, revision, url = "origin") {
  revision = revision.trim();
  log(`Syncing ${dir} to ${revision} of ${url}`);
  if (currentPlatform() == Platform.windows) {
    maybeDeleteGitLockFile(dir);
  }
  let target = revision;
  if (!hasCommit(dir, revision)) {
    const shallow =
      spawnChecked("git", ["rev-parse", "--is-shallow-repository"], {
        cwd: dir,
      })
        .stdout.toString()
        .trim() === "true";
    spawnChecked(
      "git",
      ["fetch", ...(shallow ? ["--depth", "1"] : []), url, revision],
      { cwd: dir, stdio: "inherit" },
    );
    target = "FETCH_HEAD";
  }
  spawnChecked("git", ["reset", "--hard", target], {
    cwd: dir,
    stdio: "inherit",
  });
}

// Only a commit id names the same commit everywhere; a branch is fetched.
function hasCommit(dir, revision) {
  return (
    /^[0-9a-f]{40}$/.test(revision) &&
    spawnSync("git", ["cat-file", "-e", `${revision}^{commit}`], { cwd: dir })
      .status === 0
  );
}

function enforceBackendPreludeVersion() {
  const backend = getBackendDir();

  spawnChecked("git", ["submodule", "update", "--init", "--recursive"], {
    cwd: backend,
    stdio: "inherit",
  });

  const preludeVersion = fs.readFileSync(
    path.join(backend, ".preludeversion"),
    "utf8",
  );

  const prelude = path.join(backend, "prelude");
  syncRepo(prelude, preludeVersion);
}

export function updateBackendRepo() {
  const backend = getBackendDir();
  // if process.env.REPLAY_BACKEND_REV is set, use that, otherwise use the commit
  // of the REPLAY_BACKEND_REV file, which is "<commit> [<branch>]"
  const rev = process.env.REPLAY_BACKEND_REV
    ? process.env.REPLAY_BACKEND_REV
    : fs.readFileSync("REPLAY_BACKEND_REV", "utf8").trim().split(/\s+/)[0];
  syncRepo(backend, rev);
  enforceBackendPreludeVersion();
  // create a symlink to chromium in the backend checkout
  const chromiumRepoPath = process.cwd();
  const chromiumPathInBackend = path.join(backend, "chromium");
  if (fs.existsSync(chromiumPathInBackend)) {
    fs.unlinkSync(chromiumPathInBackend);
  }
  fs.symlinkSync(chromiumRepoPath, chromiumPathInBackend);
}

// The sources of a Chromium checkout: the fork at the revision being built and
// the Replay forks its DEPS pins. Split out of updateChromiumRepo so that a
// per-version checkout can be synced without running gn, which belongs in the
// build container: the paths gn bakes into the out dir have to be the
// container's, not the host's (replay_build_scripts/ci/). gclient sync is not
// run here: `gclient sync -D` only moves forward (#1107), so a per-version
// checkout's DEPS are synced once, when checkout.sh prepares it.
export function syncChromiumSources() {
  const chromium = process.cwd();
  const rev =
    process.env["CHROMIUM_REVISION"] || process.env["BUILDKITE_COMMIT"];
  syncRepo(chromium, rev);

  for (const { dir, url, revision } of getChromiumDeps()) {
    syncRepo(path.join(chromium, dir), revision, url);
  }
}

export function updateChromiumRepo() {
  syncChromiumSources();

  runGnGen();
}

// The Replay forks DEPS pins. getChromiumDeps reads their URLs and revisions.
const ReplayForkDeps = [
  "v8",
  "third_party/skia",
  "third_party/webrtc",
  "third_party/boringssl/src",
];

function getChromiumDeps() {
  const text = fs.readFileSync("DEPS", "utf8");
  return ReplayForkDeps.map((dir) => {
    const entry = new RegExp(
      `'src/${dir}':\\s*'([^']+)'\\s*\\+\\s*'@'\\s*\\+\\s*(?:Var\\('([^']+)'\\)|'([^']+)')`,
    ).exec(text);
    assert(entry, `Could not find ${dir} in DEPS`);
    const [, url, revisionVar, revision] = entry;
    if (revision) {
      return { dir, url, revision };
    }
    const value = new RegExp(`'${revisionVar}': '([^']+)'`).exec(text);
    assert(value, `Could not find ${revisionVar} in DEPS`);
    return { dir, url, revision: value[1] };
  });
}

export function getBackendDir() {
  return path.resolve(
    process.env.RECORD_REPLAY_BACKEND_DIR || path.join(__dirname, "..", ".."),
  );
}

export function getArtifactDir() {
  return path.join(
    process.env.BUILDKITE_BUILD_CHECKOUT_PATH || "./",
    "build_id",
    currentPlatform(),
    outputArchitecture(),
  );
}
