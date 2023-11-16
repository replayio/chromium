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
  cmd = "pushd chromium && node buildLinux.mjs && popd && ./chromium/replay_build_scripts/copy-artifacts.sh",
  out = 'Release',
  labels = [
    'uses_undeclared_inputs',
    'clang-module', # NOTE(dmiller): this forces buck2 to run this in build root. is necessary so that we can avoid re-symlinking in all of chromium which takes many minutes
  ],
  env = {
    "REPLAY_CHROMIUM_DOCKER_IMAGE": "$(location :chromium-build-image)",
    "RELEASE_DIR": "chromium/out/Release",
  }
)