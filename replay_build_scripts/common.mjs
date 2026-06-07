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

// Ensure a git remote named `name` points at `url` so its commits are
// fetchable. Idempotent: adds it if missing, otherwise fixes the URL.
function ensureRemote(dir, name, url) {
  try {
    spawnChecked("git", ["remote", "add", name, url], { cwd: dir, stdio: "inherit" });
  } catch (e) {
    // Remote already exists; make sure it points at the right URL.
    spawnChecked("git", ["remote", "set-url", name, url], { cwd: dir, stdio: "inherit" });
  }
}

export function syncRepo(dir, treeish, forkUrl) {
  log(`Syncing ${dir} to ${treeish}`);
  // The dep subrepos are pinned (in DEPS) to commits on the replayio forks.
  // syncRepo runs against pre-existing checkouts whose remotes may not include
  // the fork (e.g. third_party/webrtc -> replayio/webrtc-blamy, a repo distinct
  // from googlesource webrtc). Ensure the fork remote so `git fetch --all` can
  // retrieve the pinned commit, otherwise `git reset --hard` fails to parse it.
  if (forkUrl) {
    ensureRemote(dir, "replay-fork", forkUrl);
  }
  try {
    spawnChecked("git", ["fetch", "--all"], { cwd: dir, stdio: "inherit" });
  } catch (e) {
    // Ignore errors due to being at a detached head.
  }

  spawnChecked("git", ["reset", "--hard", treeish], {
    cwd: dir,
    stdio: "inherit",
  });
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

function gclient() {
  return currentPlatform() == Platform.windows ? "gclient.bat" : "gclient";
}

function runGclientSync(rev) {
  // Sync ALL chromium third-party deps (angle, abseil, dawn, perfetto, build/, ...)
  // to the revisions pinned in this commit's src/DEPS. The Replay flow only repoints
  // the 4 forks (v8/skia/webrtc/boringssl); everything else must come from gclient or
  // `gn gen` fails on stale deps after a version bump — e.g. M108-era
  // third_party/angle whose .gn predates `exec_script_allowlist`.
  //
  // This was previously disabled because the *old* M108 fork base was too stale for
  // gclient sync; the M151 base is current, so syncing against its DEPS works.
  // `--revision src@<rev>` pins the top solution to OUR commit for this sync so the
  // M151 DEPS are read regardless of the agent's .gclient managed mode (otherwise a
  // managed:true solution pinned to an old rev would reset src and sync the wrong,
  // M108-era deps). `-D` deletes removed deps; `--reset` discards stale sub-repo
  // state; hooks run (default) so generated build files exist for gn gen. The first
  // sync after the M108->M151 jump is large; later builds are incremental. Requires
  // depot_tools + a .gclient at the chromium root (solution named "src").
  const args = ["sync", "-D", "--reset"];
  if (rev) {
    args.push("--revision", `src@${rev}`);
  }
  spawnChecked(gclient(), args, { stdio: "inherit" });
}

function updateRepo(repo, treeish) {
  log(`Updating ${repo} to ${treeish}`);
  // delete git lock file if it exists on Windows
  if (currentPlatform() == Platform.windows) {
    maybeDeleteGitLockFile(repo);
  }

  syncRepo(repo, treeish);
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
  updateRepo(prelude, preludeVersion);
}

export function updateBackendRepo() {
  const backend = getBackendDir();
  // if process.env.REPLAY_BACKEND_REV is set, use that, otherwise use the REPLAY_BACKEND_REV file
  const rev = process.env.REPLAY_BACKEND_REV
    ? process.env.REPLAY_BACKEND_REV
    : fs.readFileSync("REPLAY_BACKEND_REV", "utf8").trim();
  updateRepo(backend, rev);
  enforceBackendPreludeVersion();
  // create a symlink to chromium in the backend checkout
  const chromiumRepoPath = process.cwd();
  const chromiumPathInBackend = path.join(backend, "chromium");
  if (fs.existsSync(chromiumPathInBackend)) {
    fs.unlinkSync(chromiumPathInBackend);
  }
  fs.symlinkSync(chromiumRepoPath, chromiumPathInBackend);
}

export function updateChromiumRepo() {
  const chromium = process.cwd();
  const rev =
    process.env["CHROMIUM_REVISION"] || process.env["BUILDKITE_COMMIT"];
  updateRepo(chromium, rev);

  // Bring every non-forked chromium dep to this commit's DEPS-pinned revision
  // BEFORE repointing the forks (so it can't clobber the fork checkouts, which are
  // overlaid afterwards). gclient may move the top `src` solution depending on the
  // agent's .gclient managed mode, so re-pin src to our commit afterward. (Our
  // DEPS differs from upstream M151 only in the 4 fork revs, so the non-fork deps
  // land at the correct M151 revisions regardless of which DEPS gclient read.)
  runGclientSync(rev);
  updateRepo(chromium, rev);

  const deps = getChromiumDeps();

  syncRepo(path.join(chromium, "v8"), deps.v8, "https://github.com/replayio/chromium-v8.git");

  syncRepo(path.join(chromium, "third_party", "skia"), deps.skia, "https://github.com/replayio/chromium-skia.git");

  // webrtc-blamy is a private repo; fetch over SSH (the agent has SSH access to
  // replayio privates, as for the backend). The public forks above use HTTPS.
  syncRepo(path.join(chromium, "third_party", "webrtc"), deps.webrtc, "git@github.com:replayio/webrtc-blamy.git");

  syncRepo(
    path.join(chromium, "third_party", "boringssl", "src"),
    deps.boringssl,
    "https://github.com/replayio/boringssl.git",
  );

  // (gclient sync already ran above, before the fork repointing.)
  runGnGen();
}

function getChromiumDeps() {
  const text = fs.readFileSync("DEPS", "utf8");
  let results = {
    v8: "",
    skia: "",
    webrtc: "",
    boringssl: "",
  };

  let match = /'v8_revision': '(.*?)'/.exec(text);
  assert(match, "Could not find V8 revision");
  results.v8 = match[1];

  match = /'skia_revision': '(.*?)'/.exec(text);
  assert(match, "Could not find skia revision");
  results.skia = match[1];

  match =
    /'https:\/\/github.com\/replayio\/webrtc-blamy.git' \+ '@' \+ '(.*?)'/.exec(
      text,
    );
  assert(match, "Could not find webrtc revision");
  results.webrtc = match[1];

  match = /'boringssl_revision': '(.*?)'/.exec(text);
  assert(match, "Could not find boringssl revision");
  results.boringssl = match[1];

  return results;
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
