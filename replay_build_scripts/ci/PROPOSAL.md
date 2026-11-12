# Isolating Chromium builds: step 0 for the rebase

The fork is on M108 and a rebase effort will have several Chromium versions in
flight. Today every Chromium build on CI runs in one directory on one agent,
with tooling nobody's repository describes. Two things follow from that, and both
have already happened in milder forms:

- A job that damages that directory damages every later build. A cancelled job's
  container kept building in the shared `out/Release` and every build on the
  agent then died in ninja's startup (replayio/chromium#1540, #1544). Eight of
  the last twenty-five builds were cancelled, so this is a common shape.
- A change made on the agent to try something out is invisible and
  irreversible-by-inspection. Nothing in any repository says what the agent has.

This proposes the changes that make a rebase experiment unable to damage the
pipelines everyone else depends on, and make what a job needs a property of the
repository rather than of the machine. The changes are on
`claude/ci-isolated-chromium-builds`; what still needs someone with agent access
is at the end, and so is what I could not verify.

## 1. How a Chromium build runs today

```
backend commit ──> chromium's REPLAY_BACKEND_REV ──> replay_build_scripts/build-pipeline.py
                                                      │ (driver archive on S3? else trigger build-driver-linker)
                                                      └─> trigger "chromium-build" with DRIVER_REVISION + REPLAY_BACKEND_REV
chromium-build (Buildkite)
  :pipeline: upload   curl raw.githubusercontent.com/replayio/chromium/$BRANCH/.buildkite/pipeline.yml | pipeline upload
  🔨 Chromium (linux-x86_64)      on chromium-build-1 only
  📝 Chromium Recording Tests     on the shared e2e pods, from the build id
  📝 E2E Tests                    triggers runtime-fe-end-to-end-tests
```

and afterwards, in the backend repository, `release-chromium.yml` /
`prerelease-chromium.yml` take a `BUILD_ID_SUBSET`, check the archive is on
static.replay.io and roll the fleet forward. Neither touches a checkout, so
nothing in this proposal needs a backend change.

The build step itself is a mix of host and container:

| step | runs | writes |
|---|---|---|
| `git fetch origin`, `git reset --hard origin/$BRANCH` | agent, in `/home/ubuntu/chromium/src` (skip-checkout `cd:`) | the shared checkout |
| `remove-stale-build-containers.sh` | agent | removes containers |
| `clean_artifacts.mjs` | agent | the job workspace's `build_id/` |
| `update-all-repos.mjs` | agent | the shared checkout: backend to `REPLAY_BACKEND_REV`, chromium to `BUILDKITE_COMMIT`, v8/skia/webrtc/boringssl to their DEPS revisions, then `gn gen out/Release` |
| `node buildLinux.mjs` | **container** `chromium-build-new`, with `$HOME/chromium` and `$HOME/depot_tools` mounted | `out/Release`, `base/record_replay_driver.{h,cc}` |
| `upload_build_artifacts.mjs` | agent | S3, the job workspace's `build_id/` |

So the container is where the *tools* come from, but every byte of state the
build reads and writes is the agent's, shared by every branch that builds there.
The image is not built by any pipeline: the only thing that records how
`chromium-build-new` is made is the `//BUCK` genrule (`docker build -t
chromium-build-new - < Dockerfile.build`), which nothing runs -- there is no
buck2 in this pipeline. In practice the image is whatever someone last built by
hand on the agent.

One more fact that shapes everything: of the 26 agents in the org, exactly one
has `runtimeType=chromiumbuild` -- `chromium-build-1` -- and the build step asks
for it. All 139 build jobs I looked at ran there. Isolation therefore buys
correctness, not concurrency: two milestones in flight will still take turns on
one machine (see §6).

## 2. Everything mutable a job touches outside its own workspace

| state | where | written by | reused | two versions in flight |
|---|---|---|---|---|
| the checkout | `/home/ubuntu/chromium/src` | every job's `git reset --hard` | always | each job resets the other's tree; `git reset` across a milestone is most of a rebase |
| `out/Release` | in the checkout | the build container | always | each job rebuilds what the other invalidated; a build is ~6 min warm, 10-16 min when the branch is far from the last one |
| `out/Release/args.gn` | in the checkout | **nobody** -- written once by hand per machine | always | the one piece of build configuration no checkout describes; wiping the out dir makes the next build a dcheck-less debug build without remote execution, which is why #1544 preserves it |
| generated reclient configs | `buildtools/reclient_cfgs/chromium-browser-clang/`, gitignored | gclient's `configure_reclient` hook -- **which CI never runs**, since per-build `gclient sync` is disabled in `common.mjs` | always | whatever the last manual sync produced; they name the remote worker image by digest and hold an absolute path to the deps scanner, so a wrong one silently turns remote compiles into local ones |
| the gclient solution | `/home/ubuntu/chromium/.gclient`, `.gclient_entries`, `.cipd` | `gclient sync`, by hand | always | one solution, one set of 140 dep revisions, for both versions |
| gclient-fetched toolchain | in the checkout: `third_party/llvm-build`, `build/linux/*-sysroot`, `buildtools/linux64/gn`, `third_party/ninja`, `buildtools/reclient` | `gclient sync`/hooks, by hand | always | milestones need different clang; one tree can hold one |
| reclient deps cache | `/home/ubuntu/chromium/.reproxy_cache` | reproxy | always | shared cache keyed by paths that mean different things per version |
| the backend checkout | `/home/ubuntu/chromium/backend` (+ a `chromium` symlink into the shared src) | `updateBackendRepo` | always | pinned per chromium commit, so each version wants its own |
| depot_tools | `/home/ubuntu/depot_tools`, mounted rw | whoever cloned it; depot_tools writes its own `.cipd_bin`, `python3_bin_reldir.txt`, `bootstrap-*_bin` into itself | always | one pin for all versions; a newer one drops reclient and builds stall at `[1/N]`, which is why this one is held at `1e7c5e8de` |
| the build image | `chromium-build-new` on the agent | a human | always | one image for all versions, and nothing says which Dockerfile it came from |
| EngFlow client cert and key | `/home/ubuntu/chromium/engflow.{crt,key}` | the aws-sm plugin, every job | left behind after the job, inside the directory every build container mounts | not version-specific, but it is a secret with a long life in a shared place |
| `replay_build_scripts/node_modules` | in the checkout | `npm ci`, from the *manual* branch-build pipeline only | always | the build path doesn't need it; the manual and lint paths do |
| the job's own scripts | the shared checkout | the job, by resetting it | — | a job's commands are whatever the checkout holds when they run; the cleanup in #1544 ran a previous job's version of itself |

What is already isolated, and needs nothing: the recording-test step (its own
`mktemp` workdir and `TMPDIR`, its own shallow clone of the pinned backend, the
build fetched by build id), the e2e trigger (env only), and the backend's
release pipelines (a build id and S3). `replay_build_scripts/devtools-tests.sh`
is referenced by no tracked file; `lint.mjs` is only reachable from the
commented-out preflight step and from local builds.

## 3. What node does, and what transfers

Node's pipeline (`.buildkite/runtime-node-build-and-test.yml`,
`replay_build_scripts/rbe/`) solved the same problem differently:

- **One cached checkout per version**, `/var/lib/buildkite-agent/cache/runtime-node-build/rbe-repo-$NODE_VERSION`, keyed by the version read out of the tree, for exactly the reason here: a checkout shared between node lines rebuilds everything whenever CI switches between them.
- **An image built from a Dockerfile in the repo**, tagged with a hash of that Dockerfile, so every version's branch pins its own and they coexist; a stamp inside the image makes a build refuse to run in the wrong one.
- **Compiles run remotely**, and the out dir is rebuilt from scratch whenever the toolchain image changes.
- Test steps use `mktemp` scratch dirs and remove the EngFlow cert and key in an `EXIT` trap.

What transfers: the per-version key and directory, the repo-owned image, the
stamp that keeps an out dir from outliving the image that made it, the scratch
dirs and the cert trap. All of that is in this branch.

What does not transfer:

- **The image is not the compiler.** Node's toolchain image *is* its gcc, which is why it must be published to ECR Public for EngFlow's workers to pull. Chromium compiles with the clang in `third_party/llvm-build` and links against `build/linux/debian_bullseye_amd64-sysroot`, both inside the checkout, and remote actions run in the worker image that `buildtools/reclient_cfgs/chromium-browser-clang/rewrapper_linux.cfg` names by digest -- generated from `third_party/reclient_configs`, which DEPS pins. **Chromium's remote worker image is therefore already versioned with the tree**: I regenerated that config from the local M108 tree inside the new image and got the identical digest (`gcr.io/chops-public-images-prod/rbe/siso-chromium/linux@sha256:26de9921…`). So no registry, no ECR push rights, and no backend change are needed, and the local image can be built on the agent from the repo in ~2.5 min, free afterwards.
- **Cloning the repo per job.** Node's job clones node (small). This repository is 56 GB on GitHub and ~11 GB of tracked files checked out; that is what `skip-checkout` is for. The job instead bootstraps a blobless, shallow, sparse clone of `replay_build_scripts/` and `.buildkite/` -- 72 MB, 4.4 s measured -- and runs from that.
- **`gclient`.** Node has nothing like 140 dep repositories with hooks. Creating a version's checkout is a real step with no node analogue, and it stays a prepared, human-triggered one (§5, C1).

## 4. What each change buys

**C1 -- a checkout per version, and nothing shared between versions.**
`replay_build_scripts/ci/checkout.sh` keys a directory by the milestone from
`chrome/VERSION` (`m108`), overridable per build with
`CHROMIUM_CHECKOUT_KEY`, and that directory holds the checkout, the out dir, the
gclient solution and its caches, the reclient cache, the backend checkout and the
container's `HOME`. The container mounts it at `/chromium` whatever the key is,
so gn, ninja and reclient see the same paths they always have -- moving the
checkout costs no rebuild. A build never creates a checkout and never falls back
to another version's: an unprepared key fails the job with the command to run.
Buys: a rebase branch cannot reset, rebuild or corrupt the M108 tree, and
`wipe <key>` is the whole recovery procedure.

Why the milestone and not the branch: branches on one milestone *should* share a
warm out dir (that is the difference between a 6-minute and a 16-minute build),
while a branch on another milestone must not share anything. An experiment that
wants to be alone says so with one environment variable.

**C2 -- the image comes from the repo, with depot_tools in it.**
`ci/Dockerfile` + `ci/build-image.sh`: the old `Dockerfile.build` package list
(isolating builds should not also change the build environment), the base pinned
by digest because bionic is end-of-life, and depot_tools fetched at
`1e7c5e8deb41323f762b4298a92409cb91bf1a8d` with `DEPOT_TOOLS_UPDATE=0` and its
bootstrap already run, so a build never writes to it. The tag is a hash of the
Dockerfile, so a branch that changes it gets its own image; `build.js` refuses to
build a checkout whose Dockerfile is not the image's. Buys: "the agent has the
right tools" stops being something anyone arranges, and the depot_tools pin --
today a bare clone on one machine with a comment about it in a private
`CLAUDE.md` -- becomes a line in a Dockerfile.

**C3 -- a job runs its own commit's scripts.** `ci/bootstrap.sh`, fetched by URL
at `$BUILDKITE_COMMIT` (the one thing the job takes by URL; the pipeline upload
already works this way), puts `replay_build_scripts/` on disk in 4 s. Everything
after it is repository content at the right commit, before anything touches a
checkout. Buys: the hazard behind #1544's third fix disappears, and the stale
container cleanup can run *before* the sync rather than after it.

**C4 -- the out dir's configuration comes from the repo.** `build.js` writes
`out/<dir>/args.gn` (`import("//replay_build_scripts/linux_args.gn")`) when it
is missing, stamps the out dir with the build image, and rebuilds it from
scratch when that stamp changes. Buys: wiping an out dir is safe, which is what
makes C1's recovery story true; and `args.gn` stops being the one piece of
configuration that only exists on the machines.

**C5 -- the reclient configs are generated from the tree, in the container.**
`build.js` runs `third_party/reclient_configs/configure_reclient.py` before
`gn gen`, but only inside our build image (on a developer's machine the files
belong to however they sync their tree). Buys: the remote worker image and the
deps-scanner path come from the checkout being built instead of from whenever
somebody last synced the agent by hand -- the one piece of per-version
remote-execution configuration that was drifting silently.

**C6 -- secrets stop living in the shared directory.** The cert and key are
written into the job's workspace, mounted read-only into the container from
outside the checkout, and removed in an `EXIT` trap.

**C7 -- recovery is documented and bounded.** `checkout.sh list|wipe|wipe-out`,
and `ci/README.md` as the page to read. The only writable mount in a build is
one key's directory, so there is no path by which a job writes to another
version's state. (`docker`, `/tmp` and the job workspace remain shared: a build
can still fill the disk or leave a container, which is why the stale-container
sweep stays.)

## 5. What it costs

**Build time.** Nothing in the steady state: the same incremental build in the
same warm out dir, plus 4.4 s of bootstrap clone. The image costs ~2.5 min the
first time an agent sees a new Dockerfile hash (measured: 2.5 GB image, 149 s
cold, almost all of it apt) and nothing afterwards. The first build after this
lands regenerates the reclient configs, which changes remote cache keys and so
costs one slow build. A build that switches between keys does not slow the other
key down any more -- that is the point -- but the *first* build of a newly
prepared key is a cold out dir: the worst case I can see on CI today is 15.9 min
(a branch far from the previous one, with a warm remote cache), and the local
equivalent of a cold out dir here is ~15 min. I have no measurement of a
genuinely cold CI out dir; expect 15-30 min and treat that as a hypothesis.

**Disk.** This is the real cost, and the one number I cannot check: how much
free space `chromium-build-1` has. Measured here, per version:

| | |
|---|---|
| tracked files checked out | ~11 GB |
| checkout with its gclient deps and history | 104 GB (of which `.git` 58 GB; a fresh clone is ~56 GB per GitHub) |
| `out/Release` | 4 GB (a pruned golden) to 12 GB (a working one) |

So a second milestone is roughly +60 to +115 GB depending on how its checkout is
made. `prepare --from <key>` copies a prepared tree, which costs the full size;
`git clone --reference`/`--dissociate` or a shared `gclient --cache-dir` would
cut the git part, at the price of shared (append-only) state again. If disk is
tight, the options are in that order: wipe old keys, share the object store,
add disk. **Someone with agent access needs to check free space before this is
turned on** (§6).

## 6. What needs someone with agent access, by hand

1. **Move the existing checkout into the new layout** on `chromium-build-1`, once. It costs nothing and is a rename:
   ```sh
   /home/ubuntu/chromium/src/replay_build_scripts/ci/checkout.sh adopt /home/ubuntu/chromium m108
   ```
   (After a build has run the new path once, `/home/ubuntu/depot_tools` and the
   `chromium-build-new` image are unused and can go.)
2. **Check free disk** and decide how many versions the agent will hold.
3. **The `manual-chromium-branch-build` pipeline's upload step** is stored in
   Buildkite, not in the repo, and still does
   `cd /home/ubuntu/chromium/src && git fetch --all && git reset --hard … && cd replay_build_scripts && npm ci`
   -- a second pipeline mutating the shared checkout, from a step that only needs
   to upload YAML. Replacement, in Buildkite's pipeline settings:
   ```yaml
   - label: ":pipeline: Pipeline upload"
     depends_on: "selection"
     plugins:
       - thedyrt/skip-checkout#v0.1.1: ~
     command: |
       set -euo pipefail
       curl -fsS "https://raw.githubusercontent.com/replayio/chromium/$$BUILDKITE_COMMIT/replay_build_scripts/ci/bootstrap.sh" -o ci-bootstrap.sh
       bash ci-bootstrap.sh
       cd .ci-boot
       (cd replay_build_scripts && npm ci --no-audit --no-fund)
       buildkite-agent meta-data get platform-architecture
       node replay_build_scripts/build-branch-pipeline.mjs
   ```
4. **A second build agent**, if the rebase effort wants builds of two milestones
   to run at the same time rather than in turn. Isolation does not provide that:
   one agent has the `runtimeType=chromiumbuild` tag today, and a new one needs
   its own prepared checkouts (and the disk for them).

## 7. Verified, and not

Verified here: the image builds, and against the real M108 tree mounted
read-only, `gn gen` succeeds in it (18001 targets from 3107 files) and ninja
runs 837 steps to completion -- python actions, protobuf and perfetto codegen,
libc++, `base`'s generated buildflags and the target object -- in 55 s, with
`third_party/llvm-build/Release+Asserts/bin/clang++` as the compiler; `configure_reclient.py` runs in it and reproduces the worker image digest
byte for byte, with the deps-scanner path of the mount it ran in; the bootstrap
clone (4.4 s, 72 MB) and reading `chrome/VERSION` out of it without a checkout;
the depot_tools pin fetches shallowly (1.2 s); the agent and pipeline facts, from
Buildkite's API; every piece of the state audit that is visible in the repository
or on this host.

Not verified, and worth someone's attention before this is relied on:

- **A full CI build through the new path.** Everything up to and including one
  compiled object is tested; the whole `chrome` build in a per-version checkout
  has not run. The first `-dev` build on a branch is the test.
- **`gclient sync` for a *new* milestone** in this setup. This fork disabled
  per-build `gclient sync`, so `prepare` for a milestone nobody has synced here
  is the one step I expect to need work. It is also a step the rebase has to do
  anyway.
- **What the agent's filesystem actually holds** -- free space, whether the
  generated reclient config there already carries the `/chromium/src` path (if it
  does not, remote compiles there have been falling back to local ones, which
  would show as slow builds), whether anything else in `$HOME` matters, and that
  the agent user is uid 1000 (the current setup assumes it too).
- **That `be_cmd` works in a `command:` block** as it does in a `commands:` list.
  Buildkite runs both as one generated script, so it should; a dev build says so
  in its first line if not.
