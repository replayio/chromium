const {
  getLatestRevision,
  sendBuildTestRequest,
  newTask,
} = require("../utils");

const revision = process.env.BUILD_TEST_REVISION || getLatestRevision();
const driverRevision = process.env.BUILD_TEST_DRIVER_REVISION || undefined;

if (!revision || revision.length !== 12) {
  throw new Error("Revision must be the first 12 characters of the SHA");
}

sendBuildTestRequest({
  name: `Chromium Release ${revision}`,
  tasks: [
    ...platformTasks("linux"),
    ...platformTasks("macOS"),
    ...platformTasks("windows"),
  ],
});

function platformTasks(platform) {
  const releaseTask = newTask(
    `Release Chromium ${platform}`,
    {
      kind: "ReleaseRuntime",
      runtime: "chromium",
      revision,
      driverRevision,
    },
    platform
  );
  const tasks = [releaseTask];

  if (platform == "macOS") {
    const releaseARMTask = newTask(
      `Release Chromium ${platform} ARM`,
      {
        kind: "ReleaseRuntime",
        runtime: "chromium",
        revision,
        driverRevision,
        useARM: true,
      },
      platform
    );
    tasks.push(releaseARMTask);
  }

  return tasks;
}
