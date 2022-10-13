// Sync one per-version checkout (replay_build_scripts/ci/checkout.sh sync):
// the fork to the commit being built, the Replay forks its DEPS pins, and the
// backend checkout the artifact upload reads.
//
// Unlike update-all-repos.mjs, which this replaces for CI, it works on a
// checkout somewhere else -- the key's, in $CHROMIUM_SRC_DIR -- rather than on
// the current directory, and it does not run gn: that happens in the build
// container, whose paths are the ones the out dir has to be configured for.
//
// The chromium repo is synced before the backend, so that the backend revision
// comes from the commit being built and not from whatever the checkout held
// before (updateBackendRepo falls back to the tree's REPLAY_BACKEND_REV when
// the environment does not name one).
import { syncChromiumSources, updateBackendRepo } from "../common.mjs";

const src = process.env.CHROMIUM_SRC_DIR;
if (!src) {
  console.error("sync-repos: CHROMIUM_SRC_DIR is not set");
  process.exit(1);
}
process.chdir(src);

syncChromiumSources();
updateBackendRepo();
