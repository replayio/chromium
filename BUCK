genrule(
  name = "chromium-build-image",
  srcs = ["Dockerfile.build"],
  remote = False,
  cmd = "docker build -t chromium-build-new - < Dockerfile.build && echo done > $OUT",
)

genrule(
  name = "chromium",
  srcs = glob(['**/*.*']),
  remote = False,
  cmd = 'node buildLinux.mjs && cp -r out/Release $OUT',
  out = 'out/Release/',
  labels = [
    'no_srcs_environment',
  ],
  extra_env_vars = {
    REPLAY_CHROMIUM_DOCKER_IMAGE: "$(location :chromium-build-image)",
  }
)