genrule(
  name = "chromium",
  srcs = glob(['**/*.*']),
  remote = False,
  cmd = 'docker build -t chromium-build-new - < Dockerfile.build && node buildLinux.mjs && cp -r out/Release $OUT',
  out = 'out/Release/',
  labels = [
    'no_srcs_environment',
  ],
  env = {
    "SYMEXTRACT_PATH": "$(location //symextract)",
  }
)

genrule(
  name = "esbuild-version",
  out = "version.txt",
  cmd = "$(exe //:esbuild-bin) --version > $OUT",
)