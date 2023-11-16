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
  srcs = ["chromium"]
  cmd = "node buildLinux.mjs && ./replay_build_scripts/copy-artifacts.sh",
  out = 'Release',
  env = {
    "REPLAY_CHROMIUM_DOCKER_IMAGE": "$(location :chromium-build-image)",
    "RELEASE_DIR": "out/Release",
  }
)