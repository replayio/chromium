const { spawnChecked } = require("../buildLinux");

function syncRepo(dir, treeish) {
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

exports.syncRepo = syncRepo;
