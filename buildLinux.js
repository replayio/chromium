// Script used by buildkite to build Chromium for Linux in CI
const path = require("path");
const { spawnSync } = require("child_process");
const { updateRepo } = require("./replay_build_scripts/updateRepo");

updateRepo();

const dockerArgs = [
  "run",
  "-e",
  "BUILDKITE",
  "-e",
  "GOMA_SERVER_HOST=simpsonite.goma.engflow.com",
  "-e",
  "GOMACTL_USE_PROXY=false",
  "-e",
  "DRIVER_REVISION",
  "-v",
  `${path.join(process.env.HOME, "chromium")}:/chromium`,
  "-v",
  `${path.join(process.env.HOME, "depot_tools")}:/depot_tools`,
  "-v",
  `${path.join(
    process.env.HOME,
    ".goma_client_oauth2_config"
  )}:/home/ubuntu/.goma_client_oauth2_config`,
  "-p",
  "9098:9099",
  "chromium-build-new",
];

spawnChecked("docker", dockerArgs, { stdio: "inherit" });

function spawnChecked(cmd, args, options) {
  const prettyCmd = [cmd].concat(args).join(" ");
  console.error(prettyCmd);

  const rv = spawnSync(cmd, args, options);

  if (rv.status != 0 || rv.error) {
    console.error("Process failed:", rv.error || "");
    console.log(rv.stdout.toString() || "");
    console.error(rv.stderr.toString() || "");
    throw new Error(`Spawned process failed with exit code ${rv.status}`);
  }

  return rv;
}
exports.spawnChecked = spawnChecked;
