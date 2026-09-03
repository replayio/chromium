// Script used by buildkite to build Chromium for Linux in CI
import path from "path";
import { spawnChecked } from "./replay_build_scripts/common.mjs";

const dockerArgs = [
  "run",
  "-e",
  "BUILDKITE",
  "-e",
  "BUILDKITE_BRANCH",
  "-e",
  "BUILDKITE_PIPELINE_DEFAULT_BRANCH",
  "-e",
  "LOCAL_DEVELOPER_BUILD_EXTENSION",
  "-e",
  "DRIVER_REVISION",
  "-e",
  "RBE_service",
  // reproxy authenticates via the engflow_auth credential helper instead of mTLS
  // certs. the binary + imported token store both live on the mounted /chromium
  // dir; XDG_CONFIG_HOME points engflow_auth at that store inside the container.
  // see replay_build_scripts/setup_engflow_auth.sh
  "-e",
  "RBE_experimental_credentials_helper=/chromium/engflow_auth",
  "-e",
  "RBE_experimental_credentials_helper_args=get",
  "-e",
  "XDG_CONFIG_HOME=/chromium/.engflow",
  "-v",
  `${path.join(process.env.HOME, "chromium")}:/chromium`,
  "-v",
  `${path.join(process.env.HOME, "depot_tools")}:/depot_tools`,
  "chromium-build-new",
];

spawnChecked("docker", dockerArgs, { stdio: "inherit" });
