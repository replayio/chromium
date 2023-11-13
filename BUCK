genrule(
  name = "build-image",
  srcs = ['Dockerfile.build'],
  cmd = 'docker buildx build --output type=tar,dest=$OUT -t chromium-build-new - < Dockerfile.build',
  out = 'chromium-build.tar',
)

genrule(
  name = "chromium",
  srcs = glob(['**/*.*']),
  remote = False,
  cmd = 'docker load --input $(location :build-image) && node buildLinux.mjs && cp -r out/Release $OUT',
  out = 'out/Release/',
  labels = [
    'no_srcs_environment',
  ],
)