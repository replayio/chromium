// Script which sets a handler for collecting source maps from scripts in the
// recording. Runs when recording/replaying if source map collection is enabled.
(() => {

// Avoid monkey patching.
const { fetch, URL, DOMException, Error, Promise, Response, queueMicrotask } = window;
const ArrayIsArray = Array.isArray;
const DateNow = Date.now;
const { parse: JSONParse, stringify: JSONStringify } = JSON;
const ObjectCreate = Object.create;
const ObjectDefineProperty = Object.defineProperty;
const ObjectGetOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
const ObjectSetPrototypeOf = Object.setPrototypeOf;
const ReflectDeleteProperty = Reflect.deleteProperty;
const { bind, call } = Function.prototype;
const uncurryThis = bind.bind(call);
const ArrayPrototypePush = uncurryThis(Array.prototype.push);
const StringPrototypeStartsWith = uncurryThis(String.prototype.startsWith);
const uncurryGetter = (proto, key) =>
  uncurryThis(ObjectGetOwnPropertyDescriptor(proto, key).get);
const URLPrototypeToString = uncurryThis(URL.prototype.toString);
const DOMExceptionPrototypeGetMessage = uncurryGetter(DOMException.prototype, "message");
const ResponsePrototypeGetOk = uncurryGetter(Response.prototype, "ok");
const ResponsePrototypeGetStatus = uncurryGetter(Response.prototype, "status");
const ResponsePrototypeGetStatusText = uncurryGetter(Response.prototype, "statusText");
const ResponsePrototypeText = uncurryThis(Response.prototype.text);

// Awaiting this yields for one microtask. Awaiting a real promise would read
// its `constructor`, which the page can patch; `then` here is our own property.
const nextMicrotask = { then(resolve) { queueMicrotask(resolve); } };

// `await promise` reads `promise.constructor`, normally found on
// Promise.prototype where the page can redefine it. An own `constructor`
// keeps that read off the prototype.
const withOwnConstructor = promise =>
  ObjectDefineProperty(promise, "constructor", { __proto__: null, value: Promise });

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

const fetchPromiseCache = ObjectCreate(null);

// Reads `err.stack` without running the page's Error.prepareStackTrace.
//
// V8 formats a stack on its first read and calls the `prepareStackTrace` it
// finds on the built-in Error function of the realm that created the error.
// It doesn't use the `Error` global, so a replaced `window.Error` is ignored,
// and the `Error` captured above is the function V8 looks at. The lookup is an
// ordinary property read though, so the hook can also sit on Function.prototype,
// Object.prototype or a Proxy the page put in Error's prototype chain. An own
// property on Error stops the lookup before it gets there, so the hook is
// shadowed for the read rather than removed.
//
// An error created in another realm (e.g. an iframe) would consult that
// realm's Error instead. Errors reaching this script come from its own code
// or from built-ins of its own window, so that isn't expected here.
function defaultStack(err) {
  const hook = ObjectGetOwnPropertyDescriptor(Error, "prepareStackTrace");
  if (hook) {
    // Keeps Object.prototype out of the descriptor when it is handed back.
    ObjectSetPrototypeOf(hook, null);
  }
  try {
    ObjectDefineProperty(Error, "prepareStackTrace", {
      __proto__: null,
      value: undefined,
      configurable: true,
    });
  } catch {
    // The page froze Error or made its hook non-configurable.
    return undefined;
  }
  try {
    // A stack the page has already read stays in whatever form its hook gave it.
    const stack = err?.stack;
    return typeof stack === "string" ? stack : undefined;
  } finally {
    if (hook) {
      ObjectDefineProperty(Error, "prepareStackTrace", hook);
    } else {
      ReflectDeleteProperty(Error, "prepareStackTrace");
    }
  }
}

// `err.message` can be a getter the page installed on a prototype, and
// string coercion runs its Error.prototype.toString. Only an own data
// property or the built-in DOMException getter is read here.
function errorMessage(err) {
  if (typeof err === "string") return err;
  if (typeof err !== "object" || err === null) return "<no message>";
  const own = ObjectGetOwnPropertyDescriptor(err, "message");
  if (own) {
    ObjectSetPrototypeOf(own, null);
    if (typeof own.value === "string") return own.value;
  }
  try {
    return DOMExceptionPrototypeGetMessage(err);
  } catch {
    return "<no message>";
  }
}

async function fetchText(url) {
  const response = await withOwnConstructor(fetch(url));
  if (!ResponsePrototypeGetOk(response)) {
    throw new Error(`Fetching ${url} failed with status code ${ResponsePrototypeGetStatus(response)} (${ResponsePrototypeGetStatusText(response)})`);
  }
  return await withOwnConstructor(ResponsePrototypeText(response));
}

// Provide a cache for urls, salted with the supplied hash.  Practically, this
// means if the script content changes at the url, we will re-download the resource.
//
// Not async on purpose: returning a promise from an async function calls its
// `then`, looked up on Promise.prototype where the page can patch it.
function fetchTextWithCache(url, hash) {
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
    sourceMap = await withOwnConstructor(fetchTextWithCache(sourceMapURL, generatedScriptHash));
  } catch (err) {
    log(`[RuntimeError][sourcemaps] Failed to read sourcemap ${sourceMapURL}: ${errorMessage(err)}`);
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
      log(`[RuntimeError][sourcemaps] Failed to load sourcemaps from file: ${lookupName} - ${errorMessage(err)}`);
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
      sourceContent = await withOwnConstructor(fetchTextWithCache(url, generatedScriptHash));
    } catch (err) {
      log(`[RuntimeError][sourcemaps] Failed to read original source ${url}: ${errorMessage(err)}`);
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
    warning(`[RuntimeError][sourcemaps] Exception - ${defaultStack(err) || errorMessage(err)}`);
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
    logError(`Exception parsing sourcemap JSON (${mapURL}): ${errorMessage(err)}`);
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
            url = (sourceRoot[0] === "/" ? "" : "/") + sourceRoot + url;
          }
          let sourceURL;
          try {
            sourceURL = URLPrototypeToString(new URL(url, mapURL));
          } catch {
            logError("Unable to compute original source URL: " + url);
            continue;
          }

          ArrayPrototypePush(unresolvedSources, {
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
    log(`[RuntimeError] ${m} - ${defaultStack(Error())}`);
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
    log("Failed to process sourcemap url: " + errorMessage(err));
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
