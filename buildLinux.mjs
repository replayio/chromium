// Script used by buildkite to build Chromium for Linux in CI
import { spawn, spawnSync } from "child_process";
import path from "path";
import { log } from "./replay_build_scripts/common.mjs";

// The build runs in the agent's persistent ~/chromium checkout, so a build
// that outlives its job overlaps the next job's sync and ninja there. Named
// after the job so that cancelling the job can remove it.
const containerName = `chromium-build-${
  process.env.BUILDKITE_JOB_ID || process.pid
}`;

const dockerArgs = [
  "run",
  "--name",
  containerName,
  "--rm",
  // The image's CMD runs under a shell, which as PID 1 ignores SIGTERM, so
  // the build kept going after the job was cancelled. docker-init forwards
  // signals to it instead.
  "--init",
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
  "-e",
  "RBE_service_no_auth",
  "-e",
  "RBE_use_application_default_credentials",
  "-e",
  "RBE_tls_client_auth_cert=/chromium/engflow.crt",
  "-e",
  "RBE_tls_client_auth_key=/chromium/engflow.key",
  "-v",
  `${path.join(process.env.HOME, "chromium")}:/chromium`,
  "-v",
  `${path.join(process.env.HOME, "depot_tools")}:/depot_tools`,
  "chromium-build-new",
];

log("$docker " + dockerArgs.join(" "));
const build = spawn("docker", dockerArgs, { stdio: "inherit" });

// When the job is cancelled, remove the container rather than rely on the
// docker client passing the signal on before it is killed itself.
for (const signal of ["SIGINT", "SIGTERM", "SIGHUP"]) {
  process.on(signal, () => {
    log(`Received ${signal}, removing ${containerName}`);
    spawnSync("docker", ["rm", "--force", containerName], { stdio: "inherit" });
    process.exit(1);
  });
}

build.on("error", (error) => {
  console.error(`Spawning docker failed: ${error}`);
  process.exit(1);
});

build.on("exit", (code, signal) => {
  if (code !== 0) {
    console.error(`docker run failed with exit code ${code}, signal ${signal}`);
    process.exit(1);
  }
});
