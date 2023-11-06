genrule(
  name = "chromium",
  srcs = glob(['**/*.*']),
  enable_sandbox = False,
  remote = False,
  cmd = 'pwd && ls > $OUT',
  out = 'files.txt',
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