import { updateChromiumRepo, updateBackendRepo } from "./common.mjs";

// TODO just directly drive this from the pipeline
updateBackendRepo();
updateChromiumRepo();
