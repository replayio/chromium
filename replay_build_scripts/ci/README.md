# Chromium 157 CI build setup (`rebase/157/working`): design

Scope: Linux x86_64, build only, agent `chromium-build-1`, Buildkite pipeline `chromium-build`. Staged files only; nothing pushed, triggered or run on CI.

## 1. Job flow

| # | Where | Step | Script |
|---|---|---|---|
| 0 | Buildkite | settings step uploads the build commit's `.buildkite/pipeline.yml` | (pipeline settings, unchanged) |
| 1 | agent hooks | `skip-checkout`; `aws-sm` writes `engflow-m157.crt` / `engflow-m157.key` into the job workspace | `.buildkite/pipeline.yml` |
| 2 | host | blob-less sparse clone of `replay_build_scripts/ci` at the build's commit into `.ci-boot-m157`, over the agent's SSH identity (`$BUILDKITE_REPO`), each GitHub read retried | `.buildkite/pipeline.yml` |
| 3 | host | remove containers named `chromium157-build-*` | `ci/remove-stale-build-containers.sh` |
| 4 | host | guards (root identity, free disk); create `m157` + stamp if missing; remove `tmp_pack_*` of killed fetches (older than this run, open by no process) in the checkout's repositories; fetch the commit (depth 1) into `<root>/src` over the agent's SSH identity, unless it is already complete there, retried; install `.gclient` | `ci/checkout.sh sync` |
| 5 | host | build image `chromium157-build:<Dockerfile hash>-u<uid>` if absent | `ci/build-image.sh ensure` |
| 6 | container | `origin/HEAD` → `origin/main`, check the commit is complete, checkout, `gclient sync --nohooks --no-history -D`, `gclient runhooks`, re-stamp `LASTCHANGE` | `ci/container/sync.sh` |
| 7 | container | driver download + embed, image check, remote-exec check, `args.gn`, `gn gen`, `autoninja` (siso) | `build.cjs` |
| 8 | host | collect logs, failed-step list; upload `logs/**` (always) | `ci/job.sh` |
| 9 | container | `replay-chromium/` → `<build id>.tar.xz`, `build_id` | `ci/container/package.sh` |
| 9a | container | `out/Release/chrome` → `<build id>.symbols.tgz` (master's symbols archive), with the tree's node | `replay_build_scripts/symbols_archive.mjs` |
| 10 | host | upload `build_id/linux/x86_64/**`; EXIT trap removes workspace files | `ci/job.sh`, `.buildkite/pipeline.yml` |

- Commit built = HEAD of the bootstrap clone (scripts and tree cannot diverge).
  - `replay_build_scripts/ci/checkout.sh` (`sync`), `.buildkite/pipeline.yml`
- `BUILDKITE_COMMIT=HEAD` → the branch tip is fetched instead. Unreachable through `chromium-build`: its settings step refuses `HEAD`.
  - `.buildkite/pipeline.yml` (`REF`)

## 2. Files

Paths relative to the repository root. Origin: M = replayio/chromium `master`, 147 = `rebase/147/working`, 158 = BLamy/chromium `chromium-158-buildkite`, new.

| File | Purpose | Origin | Delta vs origin |
|---|---|---|---|
| `.buildkite/pipeline.yml` | one build step; test-step placeholder | M (step shape, plugins, agent tags, cert via aws-sm + EXIT trap), 158 (concurrency group) | no mac/windows/test/buildevents steps; own cert file names; `priority: -1`; timeout exported to the job; logic moved to `ci/job.sh`, except step 2 (script fetch), inline |
| `replay_build_scripts/build-pipeline.py` | no-op pipeline for `runtime-build-and-test` (`{"steps": ["wait"]}`) | M (file name, invocation contract) | no triggers, no file reads |
| `replay_build_scripts/ci/config.sh` | constants (key, root, src remote URL in the container, image repo, container prefix, disk minima) + guards `assert_isolated_root`, `require_free_gb` + retry helper `github_read` | new | — |
| `replay_build_scripts/ci/bootstrap.sh` | step 2 as a file, for steps that fetch it and run it; the `chromium-build` settings step refuses a commit without it | M | dest `.ci-boot-m157`; sparse path `ci/` only; ref is an argument; repo-local `gc.auto 0` |
| `replay_build_scripts/ci/job.sh` | job driver: stale containers → sync → build → logs → package → upload | 158 (inline pipeline command) | script instead of YAML; artifacts staged under `m157/jobs/<job>`; siso logs; default keep-going |
| `replay_build_scripts/ci/checkout.sh` | `root`, `sync`, `wipe`, `wipe-out` for the one fixed root | M + 158 | removed: version/env-derived key, `link_legacy_checkout`, `adopt`, `prepare --from`, `nearest_src`, `.git-cache`, backend checkout, delete-on-failure trap; all git moved into the container, except the src fetch |
| `replay_build_scripts/ci/container/sync.sh` | in-image: checkout of the commit the host fetched, gclient sync, hooks, `LASTCHANGE` | 158 (`checkout.sh sync` inline `bash -c`) | idempotent `git init`; no fetch; local `origin/HEAD`; default gclient jobs |
| `replay_build_scripts/ci/gclient` | `.gclient` solution; `custom_vars` for the `configure_siso` hook | 147 (`replay.gclient`), 158 (`.gclient` heredoc) | fork URL; `reapi_backend_config_path` names a file that exists; no `rbe_instance` |
| `replay_build_scripts/ci/build-image.sh` | image ref/ensure | 158 | repo `chromium157-build`; disk guard on docker root; `tag`/`build` subcommands dropped |
| `replay_build_scripts/ci/build-in-container.sh` | `docker run` wrapper | M + 158 | fixed root (no `--root`); `/ci` read-only mount; cert mount only with `--engflow`; no `.git-cache` mount; no local-driver mount; `--oom-score-adj`; in-container deadline |
| `replay_build_scripts/ci/remove-stale-build-containers.sh` | remove `chromium157-build-*` containers | 158 | prefix from `config.sh`; no key argument |
| `replay_build_scripts/symbols_archive.mjs` | `<build id>.symbols.tgz` next to the packaged build | new | calls `buildSymbolsArchive` (moved from `upload_build_artifacts.mjs` into `symbolication.mjs`, unchanged) |
| `replay_build_scripts/ci/container/package.sh` | in-image packaging | 158 (`package-artifacts.sh`), M (`copyBuildFiles` file list) | `chrome`, `locales`, `replay-assets` required; `build_id` without newline (as M) |
| `replay_build_scripts/ci/Dockerfile` | build image | 158 | `CHROMIUM_TAG=157.0.8103.0`; depot_tools `96b9c092…` (the revision 157 `DEPS` pins for `src/third_party/depot_tools`); system `gc.auto 0` |
| `build.cjs` | in-container build script | M (`build.js`: driver embed, build id, image check, `args.gn` import), 158 (`.cjs`, max-failures switch) | reclient + ninja-state healing removed; siso remote-exec check; siso flags; env parsed into one options object |
| `replay_build_scripts/base_args.gn` | gn args | 147 | `use_allocator = "none"` dropped (not a declared arg at 157) |
| `replay_build_scripts/linux_args.gn` | imports `base_args.gn` | 147 | none |
| `build/config/siso/backend_config/replay_engflow.star` | siso backend: platform properties for EngFlow | 147 (same path) | `configs` function dropped (returned `[]` for a non-Google address) |

- `use_allocator` no-op claim:
  - upstream `base/allocator/partition_allocator/partition_alloc.gni` @157.0.8103.0: `use_partition_alloc_as_malloc = use_partition_alloc && use_allocator_shim && …`; no `use_allocator` declaration
- `configs` equivalence claim:
  - siso sets `flags["reapi_address"]` to the effective address unconditionally: `siso/subcmd/ninja/config.go` (`initConfigFlags`) @ build `b521b230cb7c56c821235e498285cf8e548a73ee`
  - 147's `__configs` returns `[]` when that address is not Google's: `rebase/147/working:build/config/siso/backend_config/replay_engflow.star`
- 147 mismatch resolved: one backend file (`replay_engflow.star`, in the directory `configure_siso.py` resolves relative names against), referenced only from `ci/gclient`; applied only by the upstream hook.
  - upstream `build/config/siso/configure_siso.py` @157.0.8103.0 (`--reapi_backend_config_path`: "relative to backend_config dir or absolute path")
- `.cjs` requirement:
  - upstream `package.json` @157.0.8103.0: `"type": "module"`

### Inherited 147 files the branch should delete (not used by this setup)

| File | Reason |
|---|---|
| `buildLinux.mjs` | mounts `$HOME/chromium` and `$HOME/depot_tools` into `chromium-build-new` |
| `Dockerfile.build`, `BUCK` | (re)build the image named `chromium-build-new` (108's legacy image name) |
| `build.js` | superseded by `build.cjs`; fails under `"type": "module"` |
| `replay_build_scripts/install-siso-config.sh`, `replay.gclient`, `siso.sisorc`, `backend.star` | superseded by `ci/gclient` + hook + `build.cjs` flags; second copy of the backend config |
| `replay_build_scripts/update-all-repos.mjs`, `update-chromium-repo.mjs` | operate on the current directory's checkout + `RECORD_REPLAY_BACKEND_DIR`; steps of the 147 pipeline |

- Source of the list: `/home/domi/replay/chromium/_rebase157/merge/chromium_new.txt`
- Not touched: `buildMac.mjs`, `buildWindows.mjs`, `mac_*_args.gn`, `windows_args.gn`, `upload_build_artifacts.mjs` (except importing `buildSymbolsArchive`), `.github/workflows/*` (all `workflow_dispatch` only).

### Branch prerequisites (outside these files)

- `DEPS`: v8, skia, webrtc, boringssl → `https://github.com/replayio/{chromium-v8,chromium-skia,chromium-webrtc,boringssl}` at pushed commits; gitlinks equal (`git_dependencies = 'SYNC'`).
- Scripts committed with the executable bit (`ci/*.sh` except `config.sh`, `ci/container/*.sh`).
  - step 2 and `bootstrap.sh` end with `test -x …/job.sh`

## 3. Isolation model

| Resource | 157 | 108 | Enforcement |
|---|---|---|---|
| Checkout / out dir / gclient state / cipd cache / container HOME | `~/chromium-checkouts/m157` | `/home/ubuntu/chromium` (= `~/chromium-checkouts/m108` symlink) | constant in `config.sh`; no env override; `assert_isolated_root` |
| depot_tools | image `/depot_tools` @ `96b9c092…` | `/home/ubuntu/depot_tools`, image pin | never mounted |
| git DEPS cache | none (`--no-history`) | `~/chromium-checkouts/.git-cache` | never mounted |
| Docker image | `chromium157-build:<hash>-u<uid>` | `chromium-build:<hash>`, `chromium-build-new:latest` | `IMAGE_REPO`; no image removal anywhere |
| Containers | `chromium157-build-<job>-<pid>` | `chromium-build-<key>-<job>` | `CONTAINER_PREFIX`; removal filter `name=^/?chromium157-build-` |
| Workspace files | `engflow-m157.{crt,key}`, `.ci-boot-m157/` | `engflow.{crt,key}`, `.ci-boot/`, `build_id/` | distinct names; EXIT trap removes exactly these three |
| git config | image `/etc/gitconfig` (`safe.directory`, `gc.auto 0`); repo-local `gc.auto 0` in `.ci-boot-m157` and `<root>/src` | host `~/.gitconfig` | no `git config --global/--system` on the host |

`assert_isolated_root` (`replay_build_scripts/ci/config.sh`), called by every entry point before any use of the root:
- `CHECKOUT_ROOT` is not a symlink.
- `realpath(CHECKOUT_ROOT) == realpath($HOME)/chromium-checkouts/m157` (no symlinked parent).
- if it exists: `.replay-checkout` has `key=m157` (a directory this scheme did not create is refused, never adopted).
- Consequence: the 108 paths are never resolved, stat'ed, read or written; exclusion follows from path structure.

Mirror check (108 side cannot reach 157 objects):
- master's stale-container filters: `name=^/?chromium-build-<key>-`, `ancestor=chromium-build-new` → neither matches `chromium157-build-*`.
  - `ci-ref/master/replay_build_scripts/remove-stale-build-containers.sh`
- master's key `m108` ≠ `m157`; `link_legacy_checkout` only links `m<MAJOR of the shared tree>`.
  - `ci-ref/master/replay_build_scripts/cidir/checkout.sh`

Not present by construction: `git gc` (and auto-gc disabled where git runs), host package installs, `docker prune`/`rmi`, `cd` outside workspace and `m157/jobs/<job>`.

## 4. Host paths

`<ws>` = job workspace (`/var/lib/buildkite-agent/builds/chromium-build-1/replay/chromium-build`), `<root>` = `/home/ubuntu/chromium-checkouts/m157`.

| Path | Access | By |
|---|---|---|
| `<ws>/engflow-m157.crt`, `<ws>/engflow-m157.key` | write (aws-sm plugin), read-only bind mount, delete (EXIT trap) | pipeline step |
| `~/.ssh/**` (identity, `known_hosts`, config) | read by host `ssh` through `git`; never written | pipeline step, `checkout.sh sync` |
| `<ws>/.ci-boot-m157/**` | delete+create, read, read-only bind mount (`replay_build_scripts/ci` → `/ci`), delete | pipeline step, `build-in-container.sh` |
| `/home/ubuntu/chromium-checkouts` | `mkdir -p` (exists), `df` | `checkout.sh` |
| `<root>/.replay-checkout`, `<root>/.gclient` | write | `checkout.sh` |
| `<root>/src/.git/**` | `git init` if missing, repo-local config, stale `shallow.lock` removal, fetch of one commit, ref `refs/ci157/fetched` | `checkout.sh sync` (host) |
| `<root>/**` (`src`, `.gclient_entries`, `.cipd`, `home`) | read-write bind mount → `/chromium` | container |
| `<root>/jobs/**` | delete (previous job), create, write, read | `job.sh` |
| `<root>/src/out/Release/siso_{failure_summary,output,failed_commands.sh}` | read | `job.sh` |
| `/home/ubuntu` | `readlink -f` (path resolution only) | `config.sh` |
| docker root (`docker info`, typically `/var/lib/docker`) | `df`; image layers, build cache, container layers via the daemon | `build-image.sh`, dockerd |
| host git configuration (`/etc/gitconfig`, `~/.gitconfig`) | read implicitly by host `git`; never written | pipeline step, `checkout.sh` |

Never accessed: `/home/ubuntu/chromium`, `/home/ubuntu/depot_tools`, `/home/ubuntu/chromium-checkouts/m108`, `/home/ubuntu/chromium-checkouts/.git-cache`.

## 5. Host commands (outside the container)

| Script | Commands |
|---|---|
| pipeline step | `set`, `trap … rm -rf <3 workspace paths>`, `git --version`, `rm -rf .ci-boot-m157`, `git init`, `git remote add`, `git config` (repo-local ×5), `printf`, `timeout 900 git fetch --filter=blob:none --depth 1`, `timeout 900 git checkout -f --detach`, `sleep` (between attempts), `test -x`, `git rev-parse`, `job.sh` |
| `bootstrap.sh` (run by no 157 step) | `git --version`, `sed`, `rm -rf .ci-boot-m157`, `git init`, `git remote add`, `git config` (repo-local ×5), `printf`, `timeout 900 git fetch --filter=blob:none --depth 1`, `timeout 900 git checkout -f --detach`, `sleep`, `test -x`, `git rev-parse` |
| `remove-stale-build-containers.sh` | `docker ps --all --quiet --filter name=^/?chromium157-build-`, `docker rm --force <those ids>`, `seq`, `sleep` |
| `checkout.sh sync` | `readlink`, `dirname`, `git -C .ci-boot-m157 rev-parse HEAD`, `git -C .ci-boot-m157 config remote.origin.url`, `mkdir`, `df`, `tail`, `tr`, `sed`, `date`, `cat >`, `git init <root>/src` (if missing), `git -C <root>/src config gc.auto 0`, `find /proc/*/fd`, `rm -f <root>/src/.git/shallow.lock` (if stale), `git -C <root>/src rev-list --objects --no-walk`, `timeout --foreground 900 git -C <root>/src fetch --depth 1 <SSH url> <commit>` (if incomplete), `sleep` (between attempts), `git -C <root>/src update-ref refs/ci157/fetched`, `cp ci/gclient <root>/.gclient` |
| `build-image.sh` | `sha256sum`, `cut`, `id`, `docker image inspect`, `docker info`, `df`, `docker build --platform linux/amd64 -t chromium157-build:… <ci dir>` |
| `build-in-container.sh` | `id`, `readlink`, `docker run --name chromium157-build-… --rm --init …`, `docker rm --force <own name>` (signal handler) |
| `job.sh` | `rm -rf <root>/jobs`, `mkdir`, `touch`, `tee`, `cp`, `sed`, `wc`, `gzip`, `head`, `cat`, `buildkite-agent artifact upload` (cwd `<root>/jobs/<job>`) |
| `checkout.sh wipe` / `wipe-out` (manual only) | `rm -rf <root>` / `rm -rf <root>/src/out` |

`docker run` arguments: `-v <root>:/chromium`, `-v <ws>/.ci-boot-m157/replay_build_scripts/ci:/ci:ro`, `-w /chromium/src`, `--user <uid>:<gid>`, `-e HOME=/chromium/home`, `--cpu-shares 128`, `--oom-score-adj 500`; build only: `-v <cert>:/engflow/client.crt:ro`, `-v <key>:/engflow/client.key:ro`, `-e RBE_tls_client_auth_cert`, `-e RBE_tls_client_auth_key`; pass-through env: `BUILDKITE`, `BUILDKITE_BRANCH`, `BUILDKITE_PIPELINE_DEFAULT_BRANCH`, `LOCAL_DEVELOPER_BUILD_EXTENSION`, `DRIVER_REVISION`, `REPLAY_BUILD_MAX_FAILURES`, `REPLAY_SISO_LOCAL_JOBS`, `REPLAY_SISO_CONFIG`.

## 6. Shared with 108 builds / not shared

| Shared | Bound |
|---|---|
| Agent slot (one job at a time) | `timeout_in_minutes: 240`; `priority: -1` (queued 108 job runs first); `concurrency: 1`, group `chromium-157/linux-x86_64`; no preemption of a running job |
| CPU / RAM, only if a 157 container outlives its job | signal handlers remove the container; `timeout <job timeout>` inside the container; `--cpu-shares 128`; `--oom-score-adj 500`; next 157 job removes leftovers |
| Disk volume | refuse below 150 GB free before the first prepare, 60 GB before any build, 20 GB on the docker root before an image build; no automatic deletion outside `<root>/jobs` |
| Docker daemon, layer store, base-image pulls | additive only (new image, new containers) |
| Job workspace directory, Buildkite pipeline `chromium-build`, plugin checkouts (`skip-checkout#v0.1.1`, `aws-sm#v2.3.1`: the versions master uses) | own file names, removed on exit |
| EngFlow cluster `simpsonite.cluster.engflow.com:443`, instance `default`; secrets `engflow-certificate`, `engflow-key` | executor capacity shared; CAS / action cache content-addressed |
| Network egress | — |
| The agent's GitHub SSH identity (steps 2 and 4) | reads retried; GitHub queues authenticated reads of the fork network under load |

Not shared: checkout, out dir, gclient/cipd state, depot_tools, git object caches, container HOME (siso/vpython caches), image, container names, siso state.

## 7. Build configuration

- gn args: `use_siso = true`, `use_remoteexec = true`, `use_reclient = false`, `symbol_level = 1`, `is_debug = false`, `dcheck_always_on = false`, `use_allocator_shim = false`, `enable_backup_ref_ptr_support = false`.
  - `replay_build_scripts/base_args.gn`; `out/Release/args.gn` = `import("//replay_build_scripts/linux_args.gn")` (autoninja follows imports: depot_tools `gn_helper.py` `_gn_lines`)
- Remote execution chain:
  - `.gclient` `custom_vars` → DEPS hook `configure_siso` → `build/config/siso/.sisoenv` (`SISO_REAPI_ADDRESS`, `SISO_REAPI_INSTANCE=default`, no `SISO_PROJECT`) + `backend_config/backend.star` (copy of `replay_engflow.star`)
    - upstream `DEPS` @157.0.8103.0 (`reapi_address`, `reapi_instance`, `reapi_backend_config_path`, hook `configure_siso`)
  - `autoninja` → depot_tools `siso.py` (loads `.sisoenv`, requires `backend.star`) → `third_party/siso/cipd/siso ninja`
    - depot_tools `autoninja.py`, `siso.py` @ `96b9c092c0cdaa9f1ec87b0af777ec85977dee91`
  - mTLS: siso defaults `-reapi_tls_client_auth_cert/_key` from `$RBE_tls_client_auth_cert/_key`; a cert pair means no other credential is requested
    - `siso/reapi/reapi.go` (`RegisterFlags`, `NeedCred`), `siso/docs/environment_variables.md` @ build `b521b230…` (= `siso_version` in 157 `DEPS`)
- siso flags (CI image only): `--strict_remote=true`, `-failure_summary siso_failure_summary`, `-k <REPLAY_BUILD_MAX_FAILURES>`, optional `-local_jobs=<n>`, `-config <names>`.
  - `siso/subcmd/ninja/config.go` (`k`: "keep going until N jobs fail (0 means inifinity)"; `strict_remote`; `failure_summary`; `local_jobs`; `config`)
  - `--strict_remote` without a REAPI backend is an error: `siso/subcmd/ninja/ninja.go` ("no reapi specified, but remote is requested as --strict_remote")
- Not from 147's `siso.sisorc`: `--local_jobs=1`. Default here = siso's (CPU count); links are bounded by gn's link pool, which siso honours.
  - `siso/build/limits.go` (`Local: numCPU`), `siso/build/builder.go` (`poolSemas`)
- Step classes remote vs local: C/C++ compiles remote; mojo, blink bindings, proto, grit, nasm, typescript, v8, rust steps remote only under siso config `googlechrome`, which this backend (like 147's) does not enable → local on the agent's 4 CPUs. Switch: `REPLAY_SISO_CONFIG=googlechrome`.
  - upstream `build/config/siso/{mojo,blink_all,proto_linux,grit,nasm_linux,typescript_all,v8,rust}.star`, `config.star` @157.0.8103.0
- Pre-build refusal in the image (`build.cjs` `checkRemoteExecution`): `.sisoenv` missing, `SISO_PROJECT` set, no `SISO_REAPI_ADDRESS`, `backend.star` ≠ `replay_engflow.star`, cert/key file missing; plus image hash ≠ tree's Dockerfile hash (`checkBuildImage`).

## 8. Per-build switches (Buildkite build env)

| Variable | Default | Effect |
|---|---|---|
| `DRIVER_REVISION` | latest driver | `https://static.replay.io/downloads/linux-recordreplay-<first 12 chars>.tgz` |
| `REPLAY_BUILD_MAX_FAILURES` | `0` | `-k`: `0` keep going, `1` stop at first error, `N` stop after N |
| `REPLAY_SISO_LOCAL_JOBS` | unset | `-local_jobs=N` |
| `REPLAY_SISO_CONFIG` | unset | `-config <names>` |
| `REPLAY_BACKEND_REV` | — | unused (no backend checkout, no symbols) |

- Defaults live in `ci/job.sh`, not in a step `env` (step env overrides build env).

## 9. Artifacts (Buildkite only)

| Artifact | When |
|---|---|
| `logs/build.log.gz` | always (after the build ran) |
| `logs/failed-steps.txt` (lines `FAILED: …`, ANSI stripped) | always |
| `logs/siso_failure_summary`, `logs/siso_output.gz`, `logs/siso_failed_commands.sh` | if written by this build |
| `build_id/linux/x86_64/build_id`, `build_id/linux/x86_64/<build id>.tar.xz`, `build_id/linux/x86_64/<build id>.symbols.tgz` | build succeeded |

- Failure line format: `siso/build/cmd_output_log.go` (`"%s%s: %s %q %s\n"`, result `FAILED`).
- Not produced (vs master): S3 / static.replay.io upload, `entries`, `pak-sizes`, annotations.
- Symbols archive: the one master uploads to S3 `symbols/`, here an artifact only; the test step passes its directory to the backend as `REPLAY_SYMBOLS_DIR`.
- Build id: master's `computeBuildId`, suffix `-dev` (branch ≠ pipeline default branch).

## 10. Verification performed (local, no CI, no real docker)

- `bash -n` on all scripts; `node --check build.cjs`; YAML parse of `pipeline.yml` (anchor/alias resolve to 240).
- `job.sh` end to end against stub `docker` / `buildkite-agent` under a scratch `HOME`: failing and passing build, artifact sets, stale-container removal, image ensure.
- Guards: symlinked root, symlinked parent, unstamped root, missing cert → refused; `wipe` refused on a symlinked root.
- `build.cjs` under node 22 with stub `gn` / `autoninja` in a scratch tree with `"type": "module"`: outside-image path; in-image refusals (no `.sisoenv`, Google `.sisoenv`, wrong `backend.star`, no cert, wrong image); flag assembly.
- Host fetch of src (`checkout.sh sync`) and `sync.sh`'s checkout by sha: CI only (chromium-build), not locally.
- Source reads at pinned revisions: chromium 157.0.8103.0, siso `b521b230…`, depot_tools `96b9c092…`; fork repositories public (`gh api repos/replayio/{chromium,chromium-v8,chromium-skia,chromium-webrtc,boringssl}`).
