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

The build image's node is 8.10 and its git is 2.17 (bionic), so anything
`build.js` or these scripts run inside it has to work there: no `fs.rmSync`,
no recursive `fs.mkdirSync`, no `git sparse-checkout`.

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

The milestone is all the key knows, so two commits on one milestone with
different upstream bases look alike to it while their DEPS differ. Only the four
Replay forks move per job, so the rest of such a tree stays at whatever
`gclient sync` last left: `prepare` and `adopt` record a fingerprint of the
DEPS they synced for (every pin except those four forks) in
`.replay-checkout`, and `sync` prints a warning naming both when the commit
being built disagrees. That is the difference between build 5567's undefined
`pdf_use_skia_paths_override` and a line saying the tree was synced for other
DEPS. Set `CHROMIUM_CHECKOUT_KEY` to give such a branch a directory of its own.

Everything a key owns is under `~/chromium-checkouts/<key>/`
(`$CHROMIUM_CHECKOUTS_DIR` to move it): `src/` including `src/out/Release`, the
gclient solution and its caches, the backend checkout, and the container's
`HOME`. The container mounts that directory at `/chromium` whatever the key is,
so every path gn, ninja and reclient bake into their state is the same for every
key and the same as it was before this change.

## Disk

A Chromium checkout here is 104 GB: 58 GB of the fork's git objects, 14 GB in
the 140 repositories DEPS pins, and ~31 GB of working files. Two of the three
are git objects, which are append-only and identical whatever version checked
them out, so a second version shares them rather than copying:

- The fork's history comes from a local `git clone`, which hardlinks the pack
  files: 58 GB becomes 0.76 s and about 2 MB (measured). `prepare --from`
  hardlinks every `.git` the same way, with `rsync --link-dest`, while copying
  working files for real -- a gclient hook that overwrites one in place must not
  write into the version it was copied from.
- The DEPS repositories come from gclient's own mirror of them,
  `<checkouts dir>/.git-cache` (`GIT_CACHE_PATH`), which every version's sync
  populates and clones from. It belongs to no key, and `wipe` cannot touch it:
  a key may not begin with a dot.

So a second milestone costs its working files and its build outputs, around
35-45 GB, not another 104 GB. Hardlinked objects also keep the versions
independent, which `--reference`/alternates would not: deleting one checkout
leaves the others' objects alone, so `wipe` stays `rm -rf` of one directory.

Not shared on purpose: the working files (they differ per milestone, and a build
or a hook writes in them) and `out/Release`, which is the state this whole
arrangement exists to keep apart.

### The sharing decays, so keep an eye on it

Hardlinks share the packs that existed when the key was made. Every job then
runs a full `git fetch origin` in the fork and in each of the four Replay forks,
so each key accumulates packs of its own; and `gc` *rewrites* packs, so one
`gc --auto` firing in a key replaces shared inodes with private ones and the
volume quietly grows by another object store. `prepare` and `adopt` therefore
set `gc.auto 0` in every repository they create, and `sync` sets it again on the
key it is about to build, so repacking a key is a deliberate act. `list
--sizes` prints each key's pack count: a key with many more packs than the
others has stopped being nearly free.

### Measuring it

Use `checkout.sh list --sizes`, which runs **one** `du` over all the keys
together. `du` skips the second and later names of a hardlinked file, but only
within a single invocation -- so a `du` per key charges every one of them the
whole 58 GB object store they share, and 40 milestones look like 4 TB when they
cost a few hundred GB plus their working files. The `--- totals` line is the
honest number, and `df` next to it is the one that decides whether the next
milestone fits.

### Many versions

At 35-45 GB a key, ~40 milestones is 1.4-1.8 TB, which needs more than this
accounting. Two levers, in order of effect, neither implemented here:

- `out/Release` is 12 GB of the per-key cost and is derived: `wipe-out` for keys
  that are not in flight buys that back for one cold RBE build (~15 min).
- The working files are ~31 GB and are *not* shared, because a gclient hook
  overwriting one in place must not write into the version it was copied from.
  Copy-on-write has exactly that property: `cp --reflink` on XFS (`reflink=1`)
  or btrfs shares blocks until something writes to them. Adjacent milestones are
  measurably close -- m108 to m109 changes 36,823 of 406,218 tracked files, 0.69
  GiB of 3.3 GiB -- but that is `src` only; the DEPS trees and toolchains are
  the other ~28 GB and their overlap is unmeasured. Measure it (reflink-copy a
  key, sync it to the next milestone, then read shared vs exclusive extents)
  before sizing a volume for it.

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
so the next build builds a new image rather than rebuilding one in place under a
running build. The out dir is not thrown away when the image changes, because
the objects in it come from the checkout's clang and sysroot, not from the
image.

## Preparing a new version

```sh
CHROMIUM_CHECKOUT_KEY=m140 CHROMIUM_REVISION=<commit or branch> \
  replay_build_scripts/ci/checkout.sh prepare --from m108
```

copies the nearest prepared checkout without its build outputs, checks out the
revision the new version is for, and prints the `gclient sync` to run in the
image. That sync is the one step of a new version that is not routine: this fork
disabled per-build `gclient sync` ("may be too old for gclient sync to work
properly", `common.mjs`), so for a milestone nobody has synced here before,
expect it to need attention. It is also the step a rebase does anyway. `prepare`
refuses if the volume has little space left, because every key shares it (and so
do the backend's `linux_cpp_tests`, which run on this agent too).

A build never prepares a checkout and never falls back to another version's: if
the key has no checkout, the job fails and says what to run.

## On a build agent

Every key is a directory somebody prepared. There is no shared checkout left to
inherit one from, and a build never makes one.

There was, briefly: a key whose milestone matched whatever `/home/ubuntu/chromium`
currently held took that tree over by symlink, so the milestone the agent was
already building arrived here with nobody arranging it. That read the identity
of a 104 GB development checkout off a value any build could change. The
pipeline-upload steps of `runtime-build-and-test` and
`manual-chromium-branch-build` did `git reset --hard origin/$BUILDKITE_BRANCH`
in that tree on every build of every branch, so a push to a rebase branch left
it on another milestone, and the next build of *that* milestone claimed the
development checkout as its own: build 5567 took the m108 tree as `m110`,
reset it to an m110 commit, and died in `gn gen` on an undefined identifier in
`pdfium.gni`, because only the four Replay forks move per job and the other 136
DEPS repositories were still at m108's pins.

Two days of it left the agent with four keys and no checkouts:

```
m108 -> /home/ubuntu/chromium   Nov 12 16:08
m110 -> /home/ubuntu/chromium   Nov 14 09:17   (build 5567)
m112 -> /home/ubuntu/chromium   Nov 14 11:27
m116 -> /home/ubuntu/chromium   Nov 14 12:58
```

-- every one a symlink to the same tree, which by then held an m116 commit, and
`prepare` never run once. With one `.replay-checkout` between them: the stamp is
written at the key's root, and for a symlinked key that root resolved into the
shared tree, so the four names shared a single stamp that each new link
overwrote. It read `key=m116` under all four. A key's own record of what it is
was being rewritten by other keys.

Those upload steps now take this commit's scripts by URL instead of resetting a
checkout, `/home/ubuntu/chromium` is m108's own directory
(`adopt --move`), and the `chromium-build-new` image and
`/home/ubuntu/depot_tools` that went with it are gone.

`checkout.sh adopt [--move] <dir> [key]` is still how a tree that already exists
somewhere else becomes a key's, which is the cheap way to give a milestone a
checkout without `gclient sync`. Pass the key explicitly when the tree is not
sitting on the milestone it belongs to: the default comes from the tree the
script was run from, not from the directory being adopted.
