# Chromium CI build setup on a rebase branch: design

Scope: Linux x86_64, build only, the elastic build pool's `queue=runtime` hosts (replayio/backend `pulumi/build-machines/elastic_build_pool.ts`), Buildkite pipeline `chromium-build`.

## 1. Job flow

| # | Where | Step | Script |
|---|---|---|---|
| 0 | Buildkite | settings step uploads the build commit's `.buildkite/pipeline.yml` | (pipeline settings, unchanged) |
| 1 | agent hooks | `skip-checkout`; `aws-sm` writes `engflow.crt` / `engflow.key` into the job workspace | `.buildkite/pipeline.yml` |
| 2 | host | blob-less sparse clone of `replay_build_scripts/ci` and `chrome/VERSION` at the build's commit into `.ci-boot`, over the agent's SSH identity (`$BUILDKITE_REPO`), each GitHub read retried | `.buildkite/pipeline.yml` |
| 2a | host | the key: `MAJOR` of the clone's `chrome/VERSION`, or `CHROMIUM_CHECKOUT_KEY`, rejected unless it names a directory | `ci/config.sh` |
| 2b | host | take the key's use lock, shared, for the whole job: another script line's reclaim then leaves this checkout alone | `ci/job.sh`, `ci/checkout.sh use-lock` |
| 3 | host | remove containers named `chromium-build-<key>-*` | `ci/remove-stale-build-containers.sh` |
| 4 | host | guards (root identity); below the minimum free space, remove the least recently used checkouts and this line's images that no job is using, then refuse if it is still short; create the key's directory + stamp if missing, touch the stamp; remove `tmp_pack_*` of killed fetches (older than this run, open by no process) in the checkout's repositories; fetch the commit (depth 1) into `<root>/src` over the agent's SSH identity, unless it is already complete there, retried; install `.gclient` | `ci/checkout.sh sync` |
| 4a | host | with `CHROMIUM_CHECKOUT_FROM`, and only when this key has no checkout: that key's working files copied and every `.git` hardlinked, holding its use lock | `ci/checkout.sh` (`seed_from`) |
| 4b | host | `DRIVER_REVISION` from the synced checkout's `REPLAY_BACKEND_REV`, ignoring build env | `ci/job.sh` |
| 5 | host | build image `chromium-build:<Dockerfile hash>-u<uid>` if absent, and mark it used | `ci/build-image.sh ensure` |
| 6 | container | `origin/HEAD` → `origin/main`, check the commit is complete, checkout, `gclient sync --nohooks --no-history -D`, `gclient runhooks`, re-stamp `LASTCHANGE`. `.replay-synced` is removed before this and written after, so a sync that was killed leaves a checkout that says so | `ci/container/sync.sh`, `ci/checkout.sh` |
| 7 | container | driver download + embed, image check, remote-exec check, `args.gn`, `gn gen`, `autoninja` (siso). Refused unless `.replay-synced` is there (`--require-synced`) | `build.cjs`, `ci/build-in-container.sh` |
| 8 | host | collect logs, failed-step list; upload `logs/**` (always) | `ci/job.sh` |
| 9 | container | `replay-chromium/` → `<build id>.tar.xz`, `build_id` | `ci/container/package.sh` |
| 9a | container | `out/Release/chrome` → `<build id>.symbols.tgz` (master's symbols archive), with the tree's node | `replay_build_scripts/symbols_archive.mjs` |
| 10 | host | write `backend_rev` next to `build_id`; upload `build_id/linux/x86_64/**`; EXIT trap removes workspace files | `ci/job.sh`, `.buildkite/pipeline.yml` |

- Commit built = HEAD of the bootstrap clone (scripts and tree cannot diverge).
  - `replay_build_scripts/ci/checkout.sh` (`sync`), `.buildkite/pipeline.yml`
- `BUILDKITE_COMMIT=HEAD` → the branch tip is fetched instead. Unreachable through `chromium-build`: its settings step refuses `HEAD`.
  - `.buildkite/pipeline.yml` (`REF`)

## 2. Files

Paths relative to the repository root. Origin: M = replayio/chromium `master`, 147 = `rebase/147/working`, 158 = BLamy/chromium `chromium-158-buildkite`, new.

| File | Purpose | Origin | Delta vs origin |
|---|---|---|---|
| `.buildkite/pipeline.yml` | one build step; the record -> replay step | M (step shape, plugins, agent tags, cert via aws-sm + EXIT trap, backend pin from the build's `backend_rev` artifact), 158 (concurrency group) | no mac/windows/E2E/buildevents steps; `priority: -1` and a concurrency group; timeout exported to the job; logic moved to `ci/job.sh`, except step 2 (script fetch), inline; the record -> replay step takes the runtime and its symbols from the build's artifacts, since a rebase build is not uploaded to S3 |
| `replay_build_scripts/build-pipeline.py` | no-op pipeline for `runtime-build-and-test` (`{"steps": ["wait"]}`) | M (file name, invocation contract) | no triggers, no file reads |
| `replay_build_scripts/ci/config.sh` | the key and the paths, images and container names that follow from it, the disk minima, the guards `valid_key`, `assert_isolated_root`, `require_free_gb`, and the retry helper `github_read` | new, M (`version_major`, the `CHROMIUM_CHECKOUT_KEY` override, `valid_key`) | the key is worked out once, when the file is sourced, rather than per call |
| `replay_build_scripts/ci/bootstrap.sh` | step 2 as a file, for steps that fetch it and run it; the `chromium-build` settings step refuses a commit without it | M | sparse paths `ci/` and `chrome/VERSION` (the key), not `replay_build_scripts/` whole; ref is an argument; repo-local `gc.auto 0` |
| `replay_build_scripts/ci/job.sh` | job driver: use lock → stale containers → sync → backend pin → build → logs → package → upload | 158 (inline pipeline command), M (use lock held for the job, `DRIVER_REVISION` from the commit, `backend_rev` artifact) | script instead of YAML; artifacts staged under `<key>/jobs/<job>`; siso logs; default keep-going; the pin is read from the checkout, not a backend clone of its own |
| `replay_build_scripts/ci/checkout.sh` | `root`, `sync`, `synced`, `seed-from`, `wipe`, `wipe-out`, `reclaim`, `use-lock`, `image-used` | M + 158 | removed: `link_legacy_checkout`, `adopt`, `nearest_src`, the shared `.git-cache`, the backend checkout, the delete-on-failure trap; all git moved into the container, except the src fetch. `sync` converges from any state, and `.replay-synced` records the last one that finished. `seed_from` is M's `prepare --from`, in `sync`'s no-checkout-yet branch rather than a `prepare` of its own, and holding the source's use lock, which M does not. Taken from M unchanged in behaviour: `use_lock`, `use`, `unused`, `valid_key`, `last_used`, `remove_key`, `image_mark`, `image_used`, `lru`, `reclaim` -- the lock, trash and mark paths are M's, so each line's reclaim sees the other's jobs |
| `replay_build_scripts/ci/container/sync.sh` | in-image: checkout of the commit the host fetched, gclient sync, hooks, `LASTCHANGE` | 158 (`checkout.sh sync` inline `bash -c`) | idempotent `git init`; no fetch; local `origin/HEAD`; default gclient jobs |
| `replay_build_scripts/ci/gclient` | `.gclient` solution; `custom_vars` for the `configure_siso` hook | 147 (`replay.gclient`), 158 (`.gclient` heredoc) | fork URL; `reapi_backend_config_path` names a file that exists; no `rbe_instance` |
| `replay_build_scripts/ci/build-image.sh` | image tag/ref/ensure/build | 158, M (`image-used` on `ensure` and `build`) | the uid the image is built for is part of the tag; disk guard on docker root |
| `replay_build_scripts/ci/build-in-container.sh` | `docker run` wrapper; `--require-synced` for anything but the sync itself | M + 158 | fixed root (no `--root`); `/ci` read-only mount; cert mount only with `--engflow`; no `.git-cache` mount; no local-driver mount; `--oom-score-adj`; in-container deadline |
| `replay_build_scripts/ci/remove-stale-build-containers.sh` | remove `chromium-build-<key>-*` containers | 158 | prefix from `config.sh`, which carries the key; no key argument |
| `replay_build_scripts/symbols_archive.mjs` | `<build id>.symbols.tgz` next to the packaged build | new | calls `buildSymbolsArchive` (moved from `upload_build_artifacts.mjs` into `symbolication.mjs`, unchanged) |
| `replay_build_scripts/ci/container/package.sh` | in-image packaging | 158 (`package-artifacts.sh`), M (`copyBuildFiles` file list) | `chrome`, `locales`, `replay-assets` required; `build_id` without newline (as M) |
| `replay_build_scripts/ci/Dockerfile` | build image | 158, M (depot_tools' gsutil fetched at build time) | `CHROMIUM_TAG=157.0.8103.0`; depot_tools `96b9c092…` (the revision 157 `DEPS` pins for `src/third_party/depot_tools`); system `gc.auto 0`; no `chmod -R a+rX /depot_tools` (the image is built for the uid that runs it, so the `chown` covers it) |
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

| Resource | this version | another version | Enforcement |
|---|---|---|---|
| Checkout / out dir / gclient state / cipd cache / container HOME | `~/chromium-checkouts/<key>` | another key's directory (the `/home/ubuntu/chromium` bridge is gone: replayio/chromium#1648) | the key is the commit's milestone, or `CHROMIUM_CHECKOUT_KEY`; `valid_key` rejects anything that is not a directory name, `assert_isolated_root` the rest |
| depot_tools | image `/depot_tools` @ the revision this tree's `DEPS` pins | the other tree's image pin | never mounted |
| git DEPS cache | none (`--no-history`) | `~/chromium-checkouts/.git-cache` | never mounted, never removed (a name beginning with a dot is not a key) |
| Docker image | `chromium-build:<this Dockerfile's hash>-u<uid>` | `chromium-build:<its Dockerfile's hash>`, `chromium-tools:<hash>` | the tag is the Dockerfile's hash, so a version's builds only ever run in the image its own tree describes; reclaim can remove any of them, and docker refuses one a container uses |
| Containers | `chromium-build-<key>-<job>-<pid>` | `chromium-build-<its key>-<job>` | `CONTAINER_PREFIX` carries the key; removal filter `name=^/?chromium-build-<key>-` |
| Workspace files | `engflow.{crt,key}`, `.ci-boot/` | the same names, in the same workspace of the same pipeline | a host runs one job at a time, the `aws-sm` plugin writes the cert afresh for each, and the EXIT trap removes exactly these three |
| Free space on the shared volume | reclaims the least recently used keys and this line's images that no job is using, then refuses | the same, for its own images | `reclaim` takes a key's use lock exclusively, so a checkout a job is using is skipped and reported; this key and this tree's image are never candidates |
| git config | image `/etc/gitconfig` (`safe.directory`, `gc.auto 0`); repo-local `gc.auto 0` in `.ci-boot` and `<root>/src` | host `~/.gitconfig` | no `git config --global/--system` on the host |

`assert_isolated_root` (`replay_build_scripts/ci/config.sh`), called by every entry point before any use of the root:
- `CHECKOUT_ROOT` is not a symlink.
- `realpath(CHECKOUT_ROOT) == realpath($HOME)/chromium-checkouts/<key>` (no symlinked parent).
- if it exists: `.replay-checkout` has `key=<key>` (a directory this scheme did not create is refused, never adopted).
- `<key>` itself is a directory name and nothing else: `valid_key` holds for the
  milestone and for an overriding `CHROMIUM_CHECKOUT_KEY`, so no value of it
  reaches outside the checkouts directory or names one of the dot-prefixed files
  beside the keys.
- Consequence: another version's paths are never resolved, stat'ed, read or written by a build; exclusion follows from path structure.

Reclaim is the one exception. A pool host's volume holds every version that has
built on it, so a job short of space removes the least recently used keys and
its own line's images that no job is using. Both lines do this, to each other's
keys as much as their own, and a removal needs the key's use lock exclusively,
so neither can take a checkout the other is building in.

Mirror check (another version's job cannot reach this one's objects):
- master's stale-container filter is `name=^/?chromium-build-<its key>-`, and the
  keys are milestones, so it does not match this version's containers. Run by
  hand with no key it filters on `chromium-build-` alone and would match them;
  master's own step always passes its key.
  - `master:replay_build_scripts/remove-stale-build-containers.sh`
- Keys differ whenever the milestones do, and `link_legacy_checkout` is gone (replayio/chromium#1648).
  - `master:replay_build_scripts/ci/checkout.sh`
- master's reclaim removes this key when its volume runs low, and cannot while a
  job holds that key's use lock, which `job.sh` holds for the whole job. `sync`
  converges from any state, so the next build fetches and syncs again.
  - `master:replay_build_scripts/ci/checkout.sh` (`lru`, `unused`)

Not present by construction: `git gc` (and auto-gc disabled where git runs), host package installs, `docker prune`, `docker rmi` of another repository's images, `cd` outside workspace and `<key>/jobs/<job>`.

## 4. Host paths

`<ws>` = job workspace (`/var/lib/buildkite-agent/builds/<agent>/replay/chromium-build`), `<root>` = `$HOME/chromium-checkouts/<key>`, `<keys>` = `$HOME/chromium-checkouts`.

| Path | Access | By |
|---|---|---|
| `<ws>/engflow.crt`, `<ws>/engflow.key` | write (aws-sm plugin), read-only bind mount, delete (EXIT trap) | pipeline step |
| `~/.ssh/**` (identity, `known_hosts`, config) | read by host `ssh` through `git`; never written | pipeline step, `checkout.sh sync` |
| `<ws>/.ci-boot/**` | delete+create, read, read-only bind mount (`replay_build_scripts/ci` → `/ci`), delete | pipeline step, `build-in-container.sh` |
| `<keys>` | `mkdir -p` (exists), `df`, `flock` on the directory (one reclaim at a time) | `checkout.sh` |
| `<keys>/.<key>.use` | create, `flock` shared for the job and for `sync`, exclusive before a removal | `job.sh`, `checkout.sh` |
| `<keys>/.images/<image ref>` | create, `touch` | `build-image.sh` → `checkout.sh image-used` |
| `<keys>/.<other key>.use`, `<keys>/<other key>`, `<keys>/.removing.*` | `flock` exclusive, rename away, delete -- only below the minimum free space, and only a key no job is using | `checkout.sh reclaim` |
| `<root>/.replay-checkout`, `<root>/.gclient` | write | `checkout.sh` |
| `<root>/.replay-synced` | delete before a sync, write after it, read | `checkout.sh`, `build-in-container.sh` |
| `<keys>/<other key>/**` | read, with that key's use lock held, only when this key has no checkout and `CHROMIUM_CHECKOUT_FROM` names it | `checkout.sh` (`seed_from`) |
| `<root>/src/.git/**` | `git init` if missing, repo-local config, stale `shallow.lock` removal, fetch of one commit, ref `refs/ci/fetched` | `checkout.sh sync` (host) |
| `<root>/**` (`src`, `.gclient_entries`, `.cipd`, `home`) | read-write bind mount → `/chromium` | container |
| `<root>/jobs/**` | delete (previous job), create, write, read | `job.sh` |
| `<root>/src/out/Release/siso_{failure_summary,output,failed_commands.sh}` | read | `job.sh` |
| `/home/ubuntu` | `readlink -f` (path resolution only) | `config.sh` |
| docker root (`docker info`, typically `/var/lib/docker`) | `df`; image layers, build cache, container layers via the daemon | `build-image.sh`, dockerd |
| host git configuration (`/etc/gitconfig`, `~/.gitconfig`) | read implicitly by host `git`; never written | pipeline step, `checkout.sh` |

Never accessed: `/home/ubuntu/chromium` and `/home/ubuntu/depot_tools` (both gone from the agents), `<keys>/.git-cache` (not a key: reclaim skips every name beginning with a dot except its own trash). Another key's directory is read for its stamp's and out dir's mtime, and removed whole, only by `reclaim`, and only when no job holds its use lock.

## 5. Host commands (outside the container)

| Script | Commands |
|---|---|
| pipeline step | `set`, `trap … rm -rf <3 workspace paths>`, `git --version`, `rm -rf .ci-boot`, `git init`, `git remote add`, `git config` (repo-local ×5), `printf`, `timeout 900 git fetch --filter=blob:none --depth 1`, `timeout 900 git checkout -f --detach`, `sleep` (between attempts), `test -x`, `git rev-parse`, `job.sh` |
| `bootstrap.sh` (run by no step here) | `git --version`, `sed`, `rm -rf .ci-boot`, `git init`, `git remote add`, `git config` (repo-local ×5), `printf`, `timeout 900 git fetch --filter=blob:none --depth 1`, `timeout 900 git checkout -f --detach`, `sleep`, `test -x`, `git rev-parse` |
| `remove-stale-build-containers.sh` | `docker ps --all --quiet --filter name=^/?chromium-build-<key>-`, `docker rm --force <those ids>`, `seq`, `sleep` |
| `checkout.sh sync` | `readlink`, `dirname`, `git -C .ci-boot rev-parse HEAD`, `git -C .ci-boot config remote.origin.url`, `mkdir`, `df`, `tail`, `tr`, `sed`, `date`, `cat >`, `git init <root>/src` (if missing), `git -C <root>/src config gc.auto 0`, `find /proc/*/fd`, `rm -f <root>/src/.git/shallow.lock` (if stale), `git -C <root>/src rev-list --objects --no-walk`, `timeout --foreground 900 git -C <root>/src fetch --depth 1 <SSH url> <commit>` (if incomplete), `sleep` (between attempts), `git -C <root>/src update-ref refs/ci/fetched`, `cp ci/gclient <root>/.gclient` |
| `build-image.sh` | `sha256sum`, `cut`, `id`, `docker image inspect`, `docker info`, `df`, `docker build --platform linux/amd64 -t chromium-build:… <ci dir>`, `checkout.sh image-used` |
| `checkout.sh seed-from` (from `sync` with `CHROMIUM_CHECKOUT_FROM`, or by hand) | `flock` (this key's and the source's, shared), `rsync -a --delete --exclude …`, `rsync -a --link-dest=<source> --include '.git/***'` |
| `checkout.sh reclaim` (from `sync`, or by hand) | `df`, `flock` (on `<keys>`, and per key), `find -printf %T@`, `stat -c %Y`, `mktemp -d <keys>/.removing.XXXXXX`, `mv <keys>/<key> <trash>/`, `rm -rf <trash>`, `docker image ls --format … chromium-build`, `docker image rm <that repository's refs>` |
| `build-in-container.sh` | `id`, `readlink`, `docker run --name chromium-build-<key>-… --rm --init …`, `docker rm --force <own name>` (signal handler) |
| `job.sh` | `flock -s <keys>/.<key>.use`, `rm -rf <root>/jobs`, `mkdir`, `touch`, `awk` (`<root>/src/REPLAY_BACKEND_REV`), `tee`, `cp`, `sed`, `wc`, `gzip`, `head`, `cat`, `buildkite-agent artifact upload` (cwd `<root>/jobs/<job>`) |
| `checkout.sh wipe` / `wipe-out` (manual only) | `rm -rf <root>` / `rm -rf <root>/src/out` |

`docker run` arguments: `-v <root>:/chromium`, `-v <ws>/.ci-boot/replay_build_scripts/ci:/ci:ro`, `-w /chromium/src`, `--user <uid>:<gid>`, `-e HOME=/chromium/home`, `--cpu-shares 128`, `--oom-score-adj 500`; build only: `-v <cert>:/engflow/client.crt:ro`, `-v <key>:/engflow/client.key:ro`, `-e RBE_tls_client_auth_cert`, `-e RBE_tls_client_auth_key`; pass-through env: `BUILDKITE`, `BUILDKITE_BRANCH`, `BUILDKITE_PIPELINE_DEFAULT_BRANCH`, `LOCAL_DEVELOPER_BUILD_EXTENSION`, `DRIVER_REVISION`, `REPLAY_BUILD_MAX_FAILURES`, `REPLAY_SISO_LOCAL_JOBS`, `REPLAY_SISO_CONFIG`.

## 6. Shared with 108 builds / not shared

| Shared | Bound |
|---|---|
| Agent slot (one job at a time per host, several hosts per pool) | `timeout_in_minutes: 240`; `priority: -1` (a queued production job runs first); `concurrency: 1`, group `chromium-build/m157/linux-x86_64` -- the key, written out because Buildkite resolves the group before any script runs, and bounding the key rather than the host: two builds of one key would otherwise run on two hosts, each syncing its own copy of that checkout; no preemption of a running job |
| CPU / RAM, only if a container outlives its job | signal handlers remove the container; `timeout <job timeout>` inside the container; `--cpu-shares 128`; `--oom-score-adj 500`; the next job of that key removes leftovers |
| Disk volume | below 150 GB free before the first prepare, or 60 GB before any build, remove the least recently used keys and this line's images that no job is using, then refuse if it is still short; 20 GB on the docker root before an image build. Master's jobs do the same, this key included |
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
| `REPLAY_BUILD_MAX_FAILURES` | `0` | `-k`: `0` keep going, `1` stop at first error, `N` stop after N |
| `REPLAY_SISO_LOCAL_JOBS` | unset | `-local_jobs=N` |
| `REPLAY_SISO_CONFIG` | unset | `-config <names>` |
| `CHROMIUM_CHECKOUT_KEY` | the commit's milestone (`m157`) | the checkout, image mark and container names this build uses; for a rebase experiment that wants a directory nothing else writes to |
| `CHROMIUM_CHECKOUT_FROM` | unset | a key to copy this version's first checkout from, when the host has none of it: working files copied, every `.git` hardlinked. Ignored once this key has a checkout |

- Defaults live in `ci/job.sh`, not in a step `env` (step env overrides build env).
- Not switches: `DRIVER_REVISION` and `REPLAY_BACKEND_REV`. `job.sh` sets
  `DRIVER_REVISION` from the first field of the built commit's
  `REPLAY_BACKEND_REV` file, and the record -> replay step reads the same commit
  from the build's `backend_rev` artifact, so what a build embeds and what
  replays it are properties of the commit rather than of how the build was
  started.

## 9. Artifacts (Buildkite only)

| Artifact | When |
|---|---|
| `logs/build.log.gz` | always (after the build ran) |
| `logs/failed-steps.txt` (lines `FAILED: …`, ANSI stripped) | always |
| `logs/siso_failure_summary`, `logs/siso_output.gz`, `logs/siso_failed_commands.sh` | if written by this build |
| `build_id/linux/x86_64/build_id`, `build_id/linux/x86_64/<build id>.tar.xz`, `build_id/linux/x86_64/<build id>.symbols.tgz`, `build_id/linux/x86_64/backend_rev` | build succeeded |

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

- The key: `MAJOR` of `chrome/VERSION` in a sparse clone that does not have the
  file checked out (`git show` path), `CHROMIUM_CHECKOUT_KEY` honoured, and
  `../escape` and `.git-cache` refused.
- `seed-from` between scratch checkouts with real repositories: working files
  copied and every `.git` of every nested repository hardlinked (same inode),
  the source's build outputs, container HOME, job directory and markers left
  behind, this key's own stamp kept, and the copy readable as a repository at
  the source's commit. Refused for a key that is not a directory name, for this
  key itself, for a key with no checkout, and for a destination that already has
  one, with the argument checked before the destination is created.
- `.replay-synced`: `checkout.sh synced` fails before any sync has finished and
  reports the commit after one, and `build-in-container.sh --require-synced`
  refuses a checkout without it, while the sync itself does not require one.
- `reclaim` against a scratch `$HOME/chromium-checkouts` with stub `docker` and
  `df`: state with no record of use goes first (its own trash, an image with no
  mark), then keys oldest-stamp first; a key whose use lock another process
  holds, shared, is reported and skipped and the next candidate goes instead;
  this key, this tree's image and every name beginning with a dot
  (`.git-cache`) are never candidates; nothing is removed, and nothing printed,
  while the volume is above the minimum.
- Lock, trash and image-mark paths equal master's: `<keys>/.<key>.use`,
  `<keys>/.removing.XXXXXX`, `<keys>/.images/<ref>`, with `<keys>` the same
  `$HOME/chromium-checkouts` (master also honours `CHROMIUM_CHECKOUTS_DIR`,
  which CI does not set). That is what makes each line's reclaim see the
  other's jobs.
- A build, a reclaim on a pool host, the image rebuild a changed Dockerfile
  implies, and the record -> replay step reading `backend_rev` run on CI only.
