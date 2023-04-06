// Script used by buildkite to build Chromium for Linux in CI
const path = require("path");
const { spawnChecked } = require("./replay_build_scripts/common.mjs");
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
