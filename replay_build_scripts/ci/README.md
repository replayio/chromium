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
| `bootstrap.sh` | puts this commit's scripts on disk in a few seconds, before any checkout is involved, for steps without a checkout of their own (the Chromium step gets its scripts and `chrome/VERSION` from its sparse-checkout plugin instead). |
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

Every key is a directory somebody prepared. A build never makes one, never
borrows another version's, and refuses a key that is a symlink.

There was a bridge, briefly: a key whose milestone matched whatever
`/home/ubuntu/chromium` held claimed that tree by symlink, so the milestone the
agent was already building arrived here with nobody arranging it. That read the
identity of a 104 GB development checkout off a value any build could change,
and two Buildkite pipeline-upload steps -- `runtime-build-and-test` and
`manual-chromium-branch-build`, neither in this repository -- changed it, with
`git reset --hard origin/$BUILDKITE_BRANCH` in that tree on every build of every
branch. By the time it was removed the agent had four keys, every one a symlink
to the same tree, and no prepared checkout at all; build 5567 took the m108 tree
as `m110` and died in `gn gen` on an identifier that only m108's pinned pdfium
declares.

Those upload steps now read this commit's scripts with git instead of resetting
a checkout. Removing this does not by itself end the arrangement: every branch
runs its own copy of these scripts, so a branch that has not been rebased still
has the symlink code. What ends it is the agent: once `/home/ubuntu/chromium` is
gone, every copy fails over to `require`'s message.

`checkout.sh adopt [--move] <dir> [key]` is also how a tree that already exists
somewhere else becomes a key's, which is the cheap way to give a milestone a
checkout without `gclient sync`.
