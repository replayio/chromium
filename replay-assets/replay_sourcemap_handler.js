// Script which sets a handler for collecting source maps from scripts in the
// recording. Runs when recording/replaying if source map collection is enabled.
(() => {

// Avoid monkey patching.
const { fetch, URL, Error, Response, queueMicrotask } = window;
const ArrayIsArray = Array.isArray;
const DateNow = Date.now;
const { parse: JSONParse, stringify: JSONStringify } = JSON;
const { bind, call } = Function.prototype;
const uncurryThis = bind.bind(call);
const StringPrototypeStartsWith = uncurryThis(String.prototype.startsWith);
const uncurryGetter = (proto, key) =>
  uncurryThis(Object.getOwnPropertyDescriptor(proto, key).get);
const URLPrototypeToString = uncurryThis(URL.prototype.toString);
const ResponsePrototypeGetOk = uncurryGetter(Response.prototype, "ok");
const ResponsePrototypeGetStatus = uncurryGetter(Response.prototype, "status");
const ResponsePrototypeGetStatusText = uncurryGetter(Response.prototype, "statusText");
const ResponsePrototypeText = uncurryThis(Response.prototype.text);

// Awaiting this yields for one microtask. Awaiting a real promise would read
// its `constructor`, which the page can patch; `then` here is our own property.
const nextMicrotask = { then(resolve) { queueMicrotask(resolve); } };

const {
  log,
  warning,
  getRecordingId,
  sha256DigestHex,
  writeToRecordingDirectory,
  addRecordingEvent,
  addNewScriptHandler,
  getScriptSource,
  recordingDirectoryFileExists,
  readFromRecordingDirectory,
  getRecordingFilePath,
  RECORD_REPLAY_DISABLE_SOURCEMAP_CACHE,
} = __RECORD_REPLAY_ARGUMENTS__;

const fetchPromiseCache = {};

async function fetchText(url) {
  const response = await fetch(url);
  if (!ResponsePrototypeGetOk(response)) {
    throw new Error(`Fetching ${url} failed with status code ${ResponsePrototypeGetStatus(response)} (${ResponsePrototypeGetStatusText(response)})`);
  }
  return await ResponsePrototypeText(response);
}

// Provide a cache for urls, salted with the supplied hash.  Practically, this
// means if the script content changes at the url, we will re-download the resource.
async function fetchTextWithCache(url, hash) {
  const key = `${url}:${hash}`;
  if (fetchPromiseCache[key] && !RECORD_REPLAY_DISABLE_SOURCEMAP_CACHE) {
    // Return past or on-going work item.
    return fetchPromiseCache[key];
  }

  log(`[sourcemaps] Fetching sourcemap resource ${key}`);

  const resPromise = fetchText(url);
  fetchPromiseCache[key] = resPromise;
  return resPromise;
}

addNewScriptHandler(async (scriptId, sourceURL, relativeSourceMapURL) => {
  try {
  if (!relativeSourceMapURL || StringPrototypeStartsWith(relativeSourceMapURL, "data:"))
    return;

  const recordingId = getRecordingId();
  if (!recordingId) {
    // The recording has been invalidated.
    return;
  }

  const urls = getSourceMapURLs(sourceURL, relativeSourceMapURL);
  if (!urls)
    return;

  // Yield so full-source SHA256 runs after sync script registration, not under ProcessCompileEvent.
  await nextMicrotask;

  const scriptSource = getScriptSource(scriptId);
  const generatedScriptHash = sha256DigestHex(scriptSource);

  const { sourceMapURL, sourceMapBaseURL } = urls;

  let sourceMap;
  try {
    sourceMap = await fetchTextWithCache(sourceMapURL, generatedScriptHash);
  } catch (err) {
    log(`[RuntimeError][sourcemaps] Failed to read sourcemap ${sourceMapURL}: ${err.message}`);
  }
  if (!sourceMap) {
    // Download failed or nothing there.
    return;
  }

  const id = generatedScriptHash;
  const name = `sourcemap-${id}.map`;
  const lookupName = `sourcemap-${id}.lookup`;

  let sources;
  if (recordingDirectoryFileExists(name) && recordingDirectoryFileExists(lookupName)) {
    try {
      sources = JSONParse(readFromRecordingDirectory(lookupName));
    } catch (err) {
      log(`[RuntimeError][sourcemaps] Failed to load sourcemaps from file: ${lookupName} - ${err.message}`);
    }
  }

  if (!sources) {
    // Sources changed or did not exist.
    writeToRecordingDirectory(name, sourceMap);

    sources = collectUnresolvedSourceMapResources(sourceMap, sourceMapURL);
    writeToRecordingDirectory(lookupName, stringifySources(sources));
  }

  log(`[sourcemaps] Wrote sourcemap to file. Found ${sources.length} unresolved sources for "${sourceMapURL}". Downloading...`);

  addRecordingEvent(JSONStringify({
    __proto__: null,
    kind: "sourcemapAdded",
    path: getRecordingFilePath(name),
    recordingId,
    id,
    url: sourceMapURL,
    baseURL: sourceMapBaseURL,
    targetContentHash: `sha256:${generatedScriptHash}`,
    targetURLHash: sourceURL ? makeAPIHash(sourceURL) : undefined,
    targetMapURLHash: makeAPIHash(sourceMapURL),
    timestamp: DateNow(),
  }));

  for (let i = 0; i < sources.length; i++) {
    const { offset, url } = sources[i];
    let sourceContent;
    try {
      sourceContent = await fetchTextWithCache(url, generatedScriptHash);
    } catch (err) {
      log(`[RuntimeError][sourcemaps] Failed to read original source ${url}: ${err.message}`);
    }
    if (!sourceContent) {
      // Download failed or nothing there.
      continue;
    }
    const hash = sha256DigestHex(sourceContent);
    const name = `source-${hash}`;

    if (!recordingDirectoryFileExists(name)) {
      writeToRecordingDirectory(name, sourceContent);
    }
    addRecordingEvent(JSONStringify({
      __proto__: null,
      kind: "originalSourceAdded",
      path: getRecordingFilePath(name),
      recordingId,
      parentId: id,
      parentOffset: offset,
      timestamp: DateNow(),
    }));
  }
  log(`[sourcemaps] Finished downloading ${sources.length} sources for "${sourceMapURL}".`);
  } catch (err) {
    warning(`[RuntimeError][sourcemaps] Exception - ${err?.stack || err}`);
  }
});

// JSON.stringify looks up `toJSON` on the objects and arrays it visits, which
// the page can define on their prototypes.
function stringifySources(sources) {
  let json = "[";
  for (let i = 0; i < sources.length; i++) {
    const { offset, url } = sources[i];
    json += (i ? "," : "") + JSONStringify({ __proto__: null, offset, url });
  }
  return json + "]";
}

function makeAPIHash(content) {
  assert(typeof content === "string");
  const digestHex = sha256DigestHex(content);
  return "sha256:" + digestHex;
}

function collectUnresolvedSourceMapResources(mapText, mapURL) {
  let obj;
  let sourceOffset = 0;

  function logError(msg) {
    log(`[RuntimeError][sourcemaps] ${msg} (${mapURL}:${sourceOffset})`);
  }

  try {
    obj = JSONParse(mapText);
    if (typeof obj !== "object" || !obj) {
      return [];
    }
  } catch (err) {
    logError(`Exception parsing sourcemap JSON (${mapURL}): ${err?.message || err}`);
    return [];
  }

  const unresolvedSources = [];
  if (obj.version !== 3) {
    logError("Invalid sourcemap version: " + obj.version);
    return [];
  }

  if (obj.sources != null) {
    const { sourceRoot, sources, sourcesContent } = obj;

    if (ArrayIsArray(sources)) {
      for (let i = 0; i < sources.length; i++) {
        const offset = sourceOffset++;

        if (
          !ArrayIsArray(sourcesContent) ||
          typeof sourcesContent[i] !== "string"
        ) {
          let url = sources[i];
          if (typeof sourceRoot === "string" && sourceRoot) {
            url = sourceRoot.replace(/\/?/, "/") + url;
          }
          let sourceURL;
          try {
            sourceURL = URLPrototypeToString(new URL(url, mapURL));
          } catch {
            logError("Unable to compute original source URL: " + url);
            continue;
          }

          unresolvedSources.push({
            offset,
            url: sourceURL,
          });
        }
      }
    } else {
      logError("Invalid sourcemap sources list");
    }
  }

  return unresolvedSources;
}

function assert(v, msg = "") {
  if (!v) {
    const m = `Assertion failed when handling command (${msg})`;
    log(`[RuntimeError] ${m} - ${Error().stack}`);
    throw new Error(m);
  }
}

function getSourceMapURLs(sourceURL, relativeSourceMapURL) {
  let sourceBaseURL;
  if (typeof sourceURL === "string" && isValidBaseURL(sourceURL)) {
    sourceBaseURL = sourceURL;
  } else if (window?.location?.href && isValidBaseURL(window?.location?.href)) {
    sourceBaseURL = window.location.href;
  }

  let sourceMapURL;
  try {
    sourceMapURL = URLPrototypeToString(new URL(relativeSourceMapURL, sourceBaseURL));
  } catch (err) {
    log("Failed to process sourcemap url: " + err.message);
    return null;
  }

  // If the map was a data: URL or something along those lines, we want
  // to resolve paths in the map relative to the overall base.
  const sourceMapBaseURL =
    isValidBaseURL(sourceMapURL) ? sourceMapURL : sourceBaseURL;

  return { sourceMapURL, sourceMapBaseURL };
}

function isValidBaseURL(url) {
  try {
    new URL("", url);
    return true;
  } catch {
    return false;
  }
}

})();
