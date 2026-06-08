# M151 build wiring — consuming the Replay fork branches

How a Buildkite build of branch `m151` is wired to pull the rebased fork branches, and what
still has to be provisioned. Verified by reading `replay_build_scripts/common.mjs`, `DEPS`,
`build.js`, the `*_args.gn` files, and the pipelines.

## Mechanism (important: `gclient sync` is disabled)
`replay_build_scripts/common.mjs` `getChromiumDeps()` **regex-scrapes revisions from `DEPS`**, then
`syncRepo()` does `git fetch --all` + `git reset --hard <sha>` on each pre-existing subrepo
checkout. It never runs `gclient sync` (deliberately no-op'd) and never adds git remotes. So:
- **DEPS *revisions* matter; DEPS *URLs* are ignored** by the build script.
- The subrepo's remote (on the build agent) must already carry the pinned SHA.

## Done (this PR) — fork consumption is wired
- `DEPS` pinned at the fork-branch tips (verified all 4 regexes extract them):
  - `v8_revision` → `beabe23a` (replayio/chromium-v8 `v8-m151`)
  - `skia_revision` → `95c6b851` (replayio/chromium-skia `skia-m151`)
  - `boringssl_revision` → `c08fcdd4` (replayio/boringssl `boringssl-m151`)
  - `src/third_party/webrtc` → inline `https://github.com/replayio/webrtc-blamy.git @ 91e22523`
- `common.mjs` webrtc regex repointed `chromium-webrtc.git` → `webrtc-blamy.git` (else it asserts).
- `.buildkite/pipeline.m151-oneoff.yml`: adds the fork remotes to each subrepo, seeds
  `out/Release/args.gn` from `mac_x86_64_args.gn`, and forces `use_remoteexec=false` so the one-off
  builds locally without RBE.

## Remaining prerequisites / gaps (not repo bugs — infra)
1. **Build-agent subrepo remotes.** `syncRepo` only fetches existing remotes. The agent's
   `third_party/webrtc` checkout needs the `webrtc-blamy` remote (its SHA isn't on googlesource);
   v8/skia/boringssl checkouts need the replayio fork remotes. The one-off pipeline now adds these;
   the standard `pipeline.yml` (Buck path) relies on the agent already having them. SSH remotes need
   deploy-key access.
2. **reclient / RBE (real gap for the *standard* RBE pipeline).** M151 has no `checkout_reclient`
   var (the gate is `download_reclient`, left at `'checkout_chromeos'`), and `reclient_configs` +
   the `configure_reclient` hook only run under `gclient sync` (disabled). So `src/buildtools/reclient`
   and the rewrapper configs aren't provisioned, yet every `*_args.gn` sets `use_remoteexec=true`.
   Fix for RBE builds: flip `download_reclient` truthy (or via `custom_vars`) **and** run
   `configure_reclient.py` in the pipeline. The one-off sidesteps this with `use_remoteexec=false`.
3. **Driver (functional builds).** `REPLAY_BACKEND_REV`=`68477c7e` is the M108-era driver; embedding
   it in M151 is an ABI mismatch. For compile/link validation, set `REPLAY_LOCAL_DRIVER_DIR` to a
   stub (build.js escape hatch). A **functional** build needs an M151-ABI driver published to
   `static.replay.io/downloads/<platform>-recordreplay-<rev>.tgz` and `REPLAY_BACKEND_REV` bumped.
4. **Backend dir.** `RECORD_REPLAY_BACKEND_DIR` must point at a synced backend checkout on the agent
   (`update-all-repos.mjs` calls `updateBackendRepo()`).
