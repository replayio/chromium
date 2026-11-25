# Chromium CI build setup: design

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
| 4 | host | guards (root identity); below the minimum free space, remove the least recently used checkouts and this version's images that no job is using, then refuse if it is still short; create the key's directory + stamp if missing, touch the stamp; remove `tmp_pack_*` of killed fetches (older than this run, open by no process) in the checkout's repositories; remove the lockfiles a killed git left in those repositories, failing if a process still has one open; fetch the commit (depth 1) into `<root>/src` over the agent's SSH identity, unless it is already complete there, retried; install `.gclient` | `ci/checkout.sh sync` |
| 4a | host | with `CHROMIUM_CHECKOUT_FROM`, and only when this key has no checkout: that key's working files copied and every `.git` hardlinked, holding its use lock | `ci/checkout.sh` (`seed_from`) |
| 4b | host | `DRIVER_REVISION` from the synced checkout's `REPLAY_BACKEND_REV`, ignoring build env | `ci/job.sh` |
| 5 | host | build image `chromium-build:<Dockerfile hash>-u<uid>` if absent, and mark it used | `ci/build-image.sh ensure` |
| 6 | container | `origin/HEAD` → `origin/main`, check the commit is complete, checkout, and write the DEPS revisions of Replay's forks to `.replay-dep-pins` (`gclient revinfo`) | `ci/container/sync.sh checkout` |
| 6a | host | each of those revisions a dependency already here does not have, fetched depth 1 over the agent's SSH identity, with a ref to hold it | `ci/checkout.sh` (`prefetch_fork_pins`) |
| 6b | container | `gclient sync --nohooks --no-history -D`, `gclient runhooks`, re-stamp `LASTCHANGE`. `.replay-synced` is removed before step 6 and written after this, so a sync that was killed leaves a checkout that says so | `ci/container/sync.sh deps`, `ci/checkout.sh` |
| 7 | container | driver download + embed, image check, remote-exec check, `args.gn`, `gn gen`, `autoninja` (siso). Refused unless `.replay-synced` is there (`--require-synced`) | `build.cjs`, `ci/build-in-container.sh` |
| 8 | host | collect logs, failed-step list; upload `logs/**` (always) | `ci/job.sh` |
| 9 | tools container | from the tree at the commit built: package `out/Release` as `<build id>.tar.xz`, upload it to `s3://recordreplay-us-east-2{,-dev}/builds/` and copy it to `s3://recordreplay-website/downloads/` (static.replay.io), upload `<build id>.symbols.tgz` to `symbols/`, annotate the build (download links, admin app links, build id subset), write `build_id`, the symbols archive, `pak-sizes` and `entries` into `<root>/jobs/<job>/build_id/linux/x86_64` | `ci/upload-artifacts.sh`, `ci/in-tools-image.sh`, `replay_build_scripts/upload_build_artifacts.mjs` |
| 10 | host | write `backend_rev` next to `build_id`; upload `build_id/linux/x86_64/**`; EXIT trap removes workspace files | `ci/job.sh`, `.buildkite/pipeline.yml` |

- Commit built = HEAD of the bootstrap clone (scripts and tree cannot diverge).
  - `replay_build_scripts/ci/checkout.sh` (`sync`), `.buildkite/pipeline.yml`
- `BUILDKITE_COMMIT=HEAD` → the branch tip is fetched instead. Unreachable through `chromium-build`: its settings step refuses `HEAD`.
  - `.buildkite/pipeline.yml` (`REF`)

## 2. Files

Paths relative to the repository root.

| File | Purpose |
|---|---|
| `.buildkite/pipeline.yml` | the build step and the record -> replay step: plugins, agent tags, the cert through aws-sm with an EXIT trap, the backend pin read from the build's `backend_rev` artifact. Linux only, the default priority, a concurrency group, the timeout exported to the job. The logic lives in `ci/job.sh`; only the script fetch of step 2 is inline |
| `replay_build_scripts/build-pipeline.py` | no-op pipeline for `runtime-build-and-test` (`{"steps": ["wait"]}`): no triggers, no file reads |
| `replay_build_scripts/ci/config.sh` | the key and the paths, images and container names that follow from it, the disk minima, the guards `valid_key`, `assert_isolated_root`, `require_free_gb`, and the retry helper `github_read`. The key is worked out once, when the file is sourced, rather than per call |
| `replay_build_scripts/ci/bootstrap.sh` | step 2 as a file, for steps that fetch it and run it; the `chromium-build` settings step refuses a commit without it. Sparse paths `ci/` and `chrome/VERSION` (the key), the ref an argument, repo-local `gc.auto 0` |
| `replay_build_scripts/ci/job.sh` | job driver: use lock → stale containers → sync → backend pin → build → logs → upload. Holds the use lock for the whole job, takes `DRIVER_REVISION` from the built commit, stages artifacts under `<key>/jobs/<job>`, collects siso logs, keeps going by default, and reads the backend pin from the checkout rather than from a clone of its own |
| `replay_build_scripts/ci/checkout.sh` | `root`, `sync`, `synced`, `seed-from`, `wipe`, `wipe-out`, `reclaim`, `use-lock`, `image-used`. There is no shared `.git-cache` and no backend checkout, and every git command runs in the container except the src fetch. `sync` converges from any state, and `.replay-synced` records the last one that finished. `seed_from` runs in `sync`'s no-checkout-yet branch and holds the source key's use lock. `prefetch_fork_pins` fetches a moved fork pin with the agent's identity |
| `replay_build_scripts/ci/container/sync.sh` | in-image, in two phases: `checkout` of the commit the host fetched plus the fork pins it declares, then `deps` for gclient sync, hooks and `LASTCHANGE`. Idempotent `git init`, no fetch, a local `origin/HEAD`, and the split exists so the host can fetch the pins between the phases |
| `replay_build_scripts/ci/gclient` | `.gclient` solution with the fork URL; `custom_vars` for the `configure_siso` hook, where `reapi_backend_config_path` names a file that exists and no `rbe_instance` is set |
| `replay_build_scripts/ci/build-image.sh` | image tag/ref/ensure/build, marking the image used; `--tools` for the `chromium-tools:<tag>` image. The uid the build image is built for is part of its tag, and the docker root has its own disk guard |
| `replay_build_scripts/ci/build-in-container.sh` | `docker run` wrapper; `--require-synced` for anything but the sync itself. Fixed root, `/ci` mounted read-only, the cert mounted only with `--engflow`, `--oom-score-adj`, and a deadline inside the container |
| `replay_build_scripts/ci/remove-stale-build-containers.sh` | remove `chromium-build-<key>-*` containers, with the prefix from `config.sh`, which carries the key, so it takes no key argument |
| `replay_build_scripts/upload_build_artifacts.mjs` | package, upload to S3 / static.replay.io, annotate, write `build_id/` |
| `replay_build_scripts/ci/upload-artifacts.sh` | runs `upload_build_artifacts.mjs` in the checkout, in the tools image, resolving the root with `checkout.sh root` |
| `replay_build_scripts/ci/in-tools-image.sh` | `docker run` of the tools image as the agent's user, on the host network, with `$HOME`, `<root>`, the cwd and the job workspace mounted at their host paths, the agent's `buildkite-agent`, and `BUILDKITE*`, `AWS_*`, `REPLAY_*`, `DRIVER_REVISION` … passed through |
| `replay_build_scripts/ci/tools.Dockerfile` | tools image: node, `awscli`, `binutils`, `git`, `xz-utils`. Every version that builds on a host shares one `chromium-tools:<tag>` image |
| `replay_build_scripts/ci/Dockerfile` | build image: `CHROMIUM_TAG=157.0.8103.0`, depot_tools `96b9c092…` (the revision `DEPS` pins for `src/third_party/depot_tools`) with its gsutil fetched at build time, system `gc.auto 0`, and built for the uid that runs it, so a `chown` covers what a `chmod -R` otherwise would |
| `build.cjs` | in-container build script: driver embed, build id, image check, `args.gn` import, the siso remote-exec check, the siso flags and the max-failures switch, with the environment parsed into one options object |
| `replay_build_scripts/base_args.gn` | gn args |
| `replay_build_scripts/linux_args.gn` | imports `base_args.gn` |
| `build/config/siso/backend_config/replay_engflow.star` | siso backend: platform properties for EngFlow, and no `configs` function |

- `use_allocator` no-op claim:
  - upstream `base/allocator/partition_allocator/partition_alloc.gni` @157.0.8103.0: `use_partition_alloc_as_malloc = use_partition_alloc && use_allocator_shim && …`; no `use_allocator` declaration
- No-`configs` claim: such a function would be dead code here, because siso sets `flags["reapi_address"]` to the effective address unconditionally, and a `configs` function returns nothing for an address that is not Google's.
  - `siso/subcmd/ninja/config.go` (`initConfigFlags`) @ build `b521b230cb7c56c821235e498285cf8e548a73ee`
- One backend file only (`replay_engflow.star`, in the directory `configure_siso.py` resolves relative names against), referenced from `ci/gclient` alone and applied by the upstream hook alone.
  - upstream `build/config/siso/configure_siso.py` @157.0.8103.0 (`--reapi_backend_config_path`: "relative to backend_config dir or absolute path")
- `.cjs` requirement:
  - upstream `package.json` @157.0.8103.0: `"type": "module"`

### Files in the tree this setup does not use, and should lose

| File | Reason |
|---|---|
| `buildLinux.mjs` | mounts `$HOME/chromium` and `$HOME/depot_tools` into `chromium-build-new` |
| `Dockerfile.build`, `BUCK` | (re)build an image named `chromium-build-new`, which nothing here builds in or refers to |
| `build.js` | superseded by `build.cjs`; fails under `"type": "module"` |
| `replay_build_scripts/install-siso-config.sh`, `replay.gclient`, `siso.sisorc`, `backend.star` | superseded by `ci/gclient` + hook + `build.cjs` flags; second copy of the backend config |
| `replay_build_scripts/update-all-repos.mjs`, `update-chromium-repo.mjs` | operate on the current directory's checkout plus `RECORD_REPLAY_BACKEND_DIR`; no step here runs them |

- Not touched: `buildMac.mjs`, `buildWindows.mjs`, `mac_*_args.gn`, `windows_args.gn` -- this setup builds Linux only, so the platforms they serve are outside its scope rather than superseded by it.

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
| Docker image | `chromium-build:<this Dockerfile's hash>-u<uid>`; `chromium-tools:<hash>` for the upload | `chromium-build:<its Dockerfile's hash>` | the tag is the Dockerfile's hash, so a version's builds only ever run in the image its own tree describes; reclaim can remove any of them, and docker refuses one a container uses |
| Containers | `chromium-build-<key>-<job>-<pid>` | `chromium-build-<its key>-<job>` | `CONTAINER_PREFIX` carries the key; removal filter `name=^/?chromium-build-<key>-` |
| Workspace files | `engflow.{crt,key}`, `.ci-boot/` | the same names, in the same workspace of the same pipeline | a host runs one job at a time, the `aws-sm` plugin writes the cert afresh for each, and the EXIT trap removes exactly these three |
| Free space on the shared volume | reclaims the least recently used keys and this version's images that no job is using, then refuses | the same, for its own images | `reclaim` takes a key's use lock exclusively, so a checkout a job is using is skipped and reported; this key and this tree's image are never candidates |
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
the images of its own version that no job is using. A removal needs the key's
use lock exclusively, and `job.sh` holds that lock, shared, for the whole job,
so a checkout a job is building in is skipped and reported rather than taken.
Losing a key between builds costs only time: `sync` converges from any state,
so the next build fetches and syncs again.

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
| `<root>/.replay-dep-pins` | write (container), read (host) | `ci/container/sync.sh`, `checkout.sh` |
| `<root>/src/<fork dep>/.git/**` | fetch of one commit over SSH, and a ref for it, only for a pinned revision the dependency does not have | `checkout.sh` (`prefetch_fork_pins`) |
| `<keys>/<other key>/**` | read, with that key's use lock held, only when this key has no checkout and `CHROMIUM_CHECKOUT_FROM` names it | `checkout.sh` (`seed_from`) |
| `<root>/src/.git/**` | `git init` if missing, repo-local config, fetch of one commit, ref `refs/ci/fetched` | `checkout.sh sync` (host) |
| `<root>/src/**/.git/*.lock`, `<root>/src/**/.git/refs/**/*.lock` | delete, unless a process has the file open, in which case the sync fails | `checkout.sh sync` (`remove_stale_git_locks`) |
| `<root>/**` (`src`, `.gclient_entries`, `.cipd`, `home`) | read-write bind mount → `/chromium` | container |
| `$HOME/**`, `<root>/**`, `<ws>/**` | read-write bind mounts at their host paths: `aws` reads the agent's identity and configuration from `$HOME`; `upload_build_artifacts.mjs` reads `<root>/src/out/Release`, `<root>/.gclient_entries`, writes its archives in `<root>/src` (removed once uploaded) and `build_id/` in `<root>/jobs/<job>` | tools container (`in-tools-image.sh`) |
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
| `checkout.sh sync` | `readlink`, `dirname`, `git -C .ci-boot rev-parse HEAD`, `git -C .ci-boot config remote.origin.url`, `mkdir`, `df`, `tail`, `tr`, `sed`, `date`, `cat >`, `git init <root>/src` (if missing), `git -C <root>/src config gc.auto 0`, `find /proc/*/fd`, `find <root>/src -type d -name .git`, `rm -f` of each stale lockfile, `git -C <root>/src rev-list --objects --no-walk`, `timeout --foreground 900 git -C <root>/src fetch --depth 1 <SSH url> <commit>` (if incomplete), `sleep` (between attempts), `git -C <root>/src update-ref refs/ci/fetched`, `cp ci/gclient <root>/.gclient` |
| `build-image.sh` | `sha256sum`, `cut`, `id`, `docker image inspect`, `docker info`, `df`, `docker build --platform linux/amd64 -t chromium-build:… <ci dir>`, `checkout.sh image-used` |
| `checkout.sh prefetch-pins` (from `sync`, or by hand) | `git rev-parse --verify` per pin, `timeout --foreground 300 git -C <dep> fetch --depth 1 git@github.com:replayio/… <sha>` for the ones missing, `git update-ref refs/ci/fetched`, `sleep` between up to three attempts |
| `checkout.sh seed-from` (from `sync` with `CHROMIUM_CHECKOUT_FROM`, or by hand) | `flock` (this key's and the source's, shared), `rsync -a --delete --exclude …`, `rsync -a --link-dest=<source> --include '.git/***'` |
| `checkout.sh reclaim` (from `sync`, or by hand) | `df`, `flock` (on `<keys>`, and per key), `find -printf %T@`, `stat -c %Y`, `mktemp -d <keys>/.removing.XXXXXX`, `mv <keys>/<key> <trash>/`, `rm -rf <trash>`, `docker image ls --format … chromium-build`, `docker image rm <that repository's refs>` |
| `build-in-container.sh` | `id`, `readlink`, `docker run --name chromium-build-<key>-… --rm --init …`, `docker rm --force <own name>` (signal handler) |
| `in-tools-image.sh` | `id`, `readlink`, `command -v buildkite-agent`, `docker run --name chromium-build-<key>-…-tools --rm --init --network host …`, `docker rm --force <own name>` (signal handler); inside: `curl` (the driver archive, for the build id check), `git rev-parse`/`git show` in `<root>/src`, `nm` and `tar`, `aws s3 cp` ×5, `buildkite-agent annotate` ×2 |
| `job.sh` | `flock -s <keys>/.<key>.use`, `rm -rf <root>/jobs`, `mkdir`, `touch`, `awk` (`<root>/src/REPLAY_BACKEND_REV`), `tee`, `cp`, `sed`, `wc`, `gzip`, `head`, `cat`, `in-tools-image.sh node replay_build_scripts/upload_build_artifacts.mjs` (cwd `<root>/src`), `buildkite-agent artifact upload` (cwd `<root>/jobs/<job>`) |
| `checkout.sh wipe` / `wipe-out` (manual only) | `rm -rf <root>` / `rm -rf <root>/src/out` |

`docker run` arguments: `-v <root>:/chromium`, `-v <ws>/.ci-boot/replay_build_scripts/ci:/ci:ro`, `-w /chromium/src`, `--user <uid>:<gid>`, `-e HOME=/chromium/home`, `--cpu-shares 128`, `--oom-score-adj 500`; build only: `-v <cert>:/engflow/client.crt:ro`, `-v <key>:/engflow/client.key:ro`, `-e RBE_tls_client_auth_cert`, `-e RBE_tls_client_auth_key`; pass-through env: `BUILDKITE`, `BUILDKITE_BRANCH`, `BUILDKITE_PIPELINE_DEFAULT_BRANCH`, `LOCAL_DEVELOPER_BUILD_EXTENSION`, `DRIVER_REVISION`, `REPLAY_BUILD_MAX_FAILURES`, `REPLAY_SISO_LOCAL_JOBS`, `REPLAY_SISO_CONFIG`.

## 6. Shared with other builds on the pool / not shared

| Shared | Bound |
|---|---|
| Agent slot (one job at a time per host, several hosts per pool) | `timeout_in_minutes: 240`; the default priority, so a queued build of this line is not passed over for another job on the pool, while a rebase lane lowers its own; `concurrency: 1`, group `chromium-build/m157/linux-x86_64` -- the key, written out because Buildkite resolves the group before any script runs, and bounding the key rather than the host: two builds of one key would otherwise run on two hosts, each syncing its own copy of that checkout; no preemption of a running job |
| CPU / RAM, only if a container outlives its job | signal handlers remove the container; `timeout <job timeout>` inside the container; `--cpu-shares 128`; `--oom-score-adj 500`; the next job of that key removes leftovers |
| Disk volume | below 150 GB free before the first prepare, or 60 GB before any build, remove the least recently used keys and this version's images that no job is using, then refuse if it is still short; 20 GB on the docker root before an image build. Another version's jobs reclaim on the same terms, this key included |
| Docker daemon, layer store, base-image pulls | additive only (new image, new containers) |
| Job workspace directory, Buildkite pipeline `chromium-build`, plugin checkouts (`skip-checkout#v0.1.1`, `aws-sm#v2.3.1`) | own file names, removed on exit |
| EngFlow cluster `simpsonite.cluster.engflow.com:443`, instance `default`; secrets `engflow-certificate`, `engflow-key` | executor capacity shared; CAS / action cache content-addressed |
| Network egress | — |
| S3 `recordreplay-us-east-2`, `recordreplay-us-east-2-dev` (`builds/`, `symbols/`), `recordreplay-website` (`downloads/`) | every object is named by the build id, which carries the commit, and `-dev` when the build is not of the pipeline's default branch |
| The agent's GitHub SSH identity (steps 2, 4 and 6a) | reads retried; GitHub queues authenticated reads of the fork network under load. Step 6a exists so that a moved fork pin is read with this identity rather than by gclient, which would fetch every branch of that fork anonymously |

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
- `--local_jobs` is left at siso's default (the CPU count); links are bounded by gn's link pool, which siso honours.
  - `siso/build/limits.go` (`Local: numCPU`), `siso/build/builder.go` (`poolSemas`)
- Step classes remote vs local: C/C++ compiles remote; mojo, blink bindings, proto, grit, nasm, typescript, v8, rust steps remote only under siso config `googlechrome`, which this backend does not enable → local on the agent's 4 CPUs. Switch: `REPLAY_SISO_CONFIG=googlechrome`.
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

## 9. Artifacts

| Artifact | When |
|---|---|
| `logs/build.log.gz` | always (after the build ran) |
| `logs/failed-steps.txt` (lines `FAILED: …`, ANSI stripped) | always |
| `logs/siso_failure_summary`, `logs/siso_output.gz`, `logs/siso_failed_commands.sh` | if written by this build |
| `build_id/linux/x86_64/build_id`, `build_id/linux/x86_64/<build id>.symbols.tgz`, `build_id/linux/x86_64/pak-sizes`, `build_id/linux/x86_64/entries`, `build_id/linux/x86_64/backend_rev` | build succeeded and was uploaded |

- Failure line format: `siso/build/cmd_output_log.go` (`"%s%s: %s %q %s\n"`, result `FAILED`).
- The runtime itself is on S3 / static.replay.io (`<build id>.tar.xz`). The record -> replay step has the backend download it by build id.
- Build id: `replay_build_scripts/upload_build_artifacts.mjs` (`buildIdExtension`), suffix `-dev` when the branch is not the pipeline's default branch. `main` is `chromium-build`'s default, so a build of it carries no suffix and the release pipelines take its id as it stands; a build of any other branch still gets `-dev`.

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
- The filter this applies to `gclient revinfo` output, over a sample holding a
  pinned fork, an upstream dependency, a branch-ref dependency, a short hash, a
  CIPD version and a `None`: only the pinned Replay GitHub URLs pass.
- `prefetch-pins` over scratch dependencies, with git's fetch recorded and
  failed: it fetches for the pinned revision a dependency lacks and not for one
  it already has or for a directory with no repository, rewrites the https URL
  to `git@github.com:`, asks for one commit, tries three times, reports a
  failure without failing the job, and on success leaves `refs/ci/fetched` at
  the revision. With no pins file it does nothing and prints nothing.
- Not verified, because it needs a fork pin to move on CI: that gclient then
  finds the revision present and skips its own fetch.
- `reclaim` against a scratch `$HOME/chromium-checkouts` with stub `docker` and
  `df`: state with no record of use goes first (its own trash, an image with no
  mark), then keys oldest-stamp first; a key whose use lock another process
  holds, shared, is reported and skipped and the next candidate goes instead;
  this key, this tree's image and every name beginning with a dot
  (`.git-cache`) are never candidates; nothing is removed, and nothing printed,
  while the volume is above the minimum.
- A build, a reclaim on a pool host, the image rebuild a changed Dockerfile
  implies, and the record -> replay step reading `backend_rev` run on CI only.
