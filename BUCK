genrule(
  name = "chromium-build-image",
  srcs = ["Dockerfile.build"],
  remote = False,
  cmd = "docker build -t chromium-build-new - < Dockerfile.build && echo chromium-build-new > $OUT",
  out = "image_name.txt",
  env = {
    "REPLAY_CHROMIUM_DOCKER_IMAGE_NAME": "chromium-build-new",
  },
)

genrule(
  name = "chromium",
  remote = False,
  cmd = 'pwd && cd chromium && node buildLinux.mjs && cp -r out/Release $OUT',
  out = 'out/Release/',
  labels = [
    'no_srcs_environment',
    'uses_undeclared_inputs',
    'clang-module',
  ],
  env = {
    "REPLAY_CHROMIUM_DOCKER_IMAGE": "$(location :chromium-build-image)",
  }
)