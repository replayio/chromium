genrule(
  name = "chromium",
  srcs = glob(['**/*.*']),
  enable_sandbox = False,
  remote = False,
  # cmd = 'pwd && ls && cd chromium_df9fb787d508/ && ls > $OUT',
  # out = 'files.txt',
  cmd = 'node build.js',
  labels = [
    'uses_undeclared_inputs',
    'no_srcs_environment',
    'clang-module',
  ]
)

genrule(
  name = "esbuild-version",
  out = "version.txt",
  cmd = "$(exe //:esbuild-bin) --version > $OUT",
)