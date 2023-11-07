genrule(
  name = "chromium",
  srcs = glob(['**/*.*']),
  enable_sandbox = False,
  remote = False,
  # cmd = 'pwd && ls > $OUT',
  cmd = 'docker build -t chromium-build-new - < Dockerfile.build && node buildLinux.mjs',
  out = 'out/Release/',
  labels = [
    # 'uses_undeclared_inputs',
    'no_srcs_environment',
    # 'clang-module', # run in build root (//)
  ]
)

genrule(
  name = "esbuild-version",
  out = "version.txt",
  cmd = "$(exe //:esbuild-bin) --version > $OUT",
)