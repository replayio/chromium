# Isolated Chromium builds

Every Chromium build -- on CI and here -- runs in an image this repository
describes, against a checkout that belongs to one Chromium version, and writes
nothing outside that version's own directory.

Before this, all of it was one directory on one agent: `/home/ubuntu/chromium`,
with one `src`, one `out/Release`, one gclient solution, one backend checkout and
one reclient cache, shared by every branch that ever built there, next to a
`depot_tools` clone and a `chromium-build-new` docker image that no repository
described. A cancelled job's container once kept building in that out dir and
every later build died in ninja's startup (replayio/chromium#1540, #1544), and
someone changing the agent to try something had no way to show what they had
changed.

## The pieces

| | |
|---|---|
| `Dockerfile` | the image builds run in: the old `//Dockerfile.build` packages plus depot_tools at a pin. It is not the compiler -- Chromium's clang, sysroot, gn and ninja all come from the checkout. |
| `build-image.sh` | builds that image and tags it with a hash of the Dockerfile, so a branch that changes it gets its own image and the two coexist. |
| `checkout.sh` | the per-version checkouts: which one this tree uses, syncing it, preparing a new one, and wiping one. |
| `build-in-container.sh` | runs the build (or anything else) in the image, with one version's directory as the only writable mount. |
| `bootstrap.sh` | the four seconds at the start of a job that put this commit's scripts on disk, before any checkout is involved. |
| `sync-repos.mjs`, `upload-artifacts.sh` | the pipeline's sync and upload steps, against the key's checkout instead of the current directory. |

## Keys

A key names a checkout and everything that belongs to it. By default it is the
milestone of the tree being built (`m108`), read from `chrome/VERSION`, so a
branch rebasing onto a new milestone gets a checkout of its own without anyone
arranging it.

Two branches on the same milestone share one checkout, which for development
branches is the point: they share a warm `out/Release`, which is why a build
takes about six minutes instead of a quarter of an hour. A rebase experiment
that wants to be alone, or two experiments on one milestone, set
`CHROMIUM_CHECKOUT_KEY` (in the Buildkite build, or on a branch's builds) and
get a directory nothing else writes to.

Everything a key owns is under `~/chromium-checkouts/<key>/`
(`$CHROMIUM_CHECKOUTS_DIR` to move it): `src/` including `src/out/Release`, the
gclient solution and its caches, the backend checkout, and the container's
`HOME`. The container mounts that directory at `/chromium` whatever the key is,
so every path gn, ninja and reclient bake into their state is the same for every
key and the same as it was before this change.

## Using it

```sh
replay_build_scripts/ci/checkout.sh list --sizes   # what is prepared here
replay_build_scripts/ci/checkout.sh key            # the key for this tree
replay_build_scripts/ci/checkout.sh sync           # to the commit being built
replay_build_scripts/ci/build-in-container.sh      # build it
```

To look around inside the image, or to run gclient with the pinned depot_tools:

```sh
replay_build_scripts/ci/build-in-container.sh -- bash
```

## Recovering

A damaged checkout is one directory, and nothing else was damaged with it:

```sh
replay_build_scripts/ci/checkout.sh wipe-out m108   # just the build outputs
replay_build_scripts/ci/checkout.sh wipe m108       # all of that version's state
replay_build_scripts/ci/checkout.sh prepare         # make it again
```

`out/Release` comes back configured from the repository (`build.js` writes its
`args.gn` from `replay_build_scripts/*_args.gn`), so wiping it no longer risks
the next build being a debug build without remote execution, which is what made
the recovery in replayio/chromium#1544 preserve the file instead.

A bad image is the same: change the `Dockerfile`, and the tag changes with it,
so the next build builds a new image and the out dir it produced is rebuilt
(`build.js` stamps the out dir with the image it was built in). Nothing is ever
rebuilt in place under a running build.

## Preparing a new version

```sh
CHROMIUM_CHECKOUT_KEY=m140 replay_build_scripts/ci/checkout.sh prepare --from m108
```

copies the nearest prepared checkout without its build outputs and leaves
`gclient sync` to run in the image, which is the one step of a new version that
is not routine: this fork disabled per-build `gclient sync` ("may be too old for
gclient sync to work properly", `common.mjs`), so for a milestone nobody has
synced here before, expect it to need attention. It is also the step a rebase
does anyway.

A build never prepares a checkout and never falls back to another version's: if
the key has no checkout, the job fails and says what to run.
