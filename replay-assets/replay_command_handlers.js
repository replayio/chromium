(() => {
// Script which defines handlers for recorder commands, 
// and usually is only loaded while replaying.

// Stored before any page script can reassign them.
const { DOMException, Error, Map, Set, String, URL } = window;
const EmptyArray = Object.freeze([]); // reduce unnecessary mem churn

const Verbose = false;
const VerboseCommands = Verbose;

const {
  log: log_,
  logTrace: logTrace_,
  warning: warning_,
  fromJsIsReplayScriptAlive: isReplayScriptAlive,
  hasDiverged,
  setCDPMessageCallback,
  sendCDPMessage: sendCDPMessageRaw,
  setCommandCallback,
  setClearPauseDataCallback,
  addNewScriptHandler,
  getCurrentError,
  forceCheckpoint,

  layoutDom,

  fromJsMakeDebuggeeValue,
  fromJsGetArgumentsInFrame,
  fromJsGetObjectByCdpId,
  fromJsIsBlinkObject,
  fromJsIsBlinkNodeObject,
  fromJsIsBlinkElementObject,
  fromJsIsBlinkCSSStyleDeclarationObject,
  fromJsHasReturnValue,
  fromJsGetReturnValue,
  fromJsGetNodeIdByCpdId,
  fromJsGetBoxModel,
  fromJsGetMatchedStylesForElement,
  fromJsCssGetStylesheetByCpdId,
  fromJsCollectEventListeners,
  fromJsDomPerformSearch,
  fromJsGetDevicePixelRatio,

  // network
  getCurrentNetworkRequestEvent,
  getCurrentNetworkStreamData,

  // constants
  CDPERROR_MISSINGCONTEXT,
  CDPERROR_NOTALIVE,
  REPLAY_CDT_PAUSE_OBJECT_GROUP,

  // for testing
  forTestingSerializeValueToArray

} = __RECORD_REPLAY_ARGUMENTS__;

///////////////////////////////////////////////////////////////////////////////
// utils.js
///////////////////////////////////////////////////////////////////////////////

// Some of these are duplicated in gSourceMapScript, so watch out when making
// modifications to update both versions...

function isFunction(val) {
  return typeof val === "function";
}

function isObject(val) {
  return !!val && (typeof val === "object" || isFunction(val))
}

// eslint-disable-next-line no-unused-vars
function isNonNullObject(obj) {
  return obj && (typeof obj == "object" || typeof obj == "function");
}

function typeofMaybeNull(value) {
  if (value === null) {
    return "null";
  }
  return typeof value;
}

function log(...args) {
  log_(ArrayPrototypeJoin(args, ' '));
}

// eslint-disable-next-line no-unused-vars
function logTrace(...args) {
  logTrace_(ArrayPrototypeJoin(args, ' '));
}

function warning(...args) {
  warning_(ArrayPrototypeJoin(args, ' '));
}

function assert(v, msg = "") {
  if (!v) {
    const m = `Assertion failed when handling command (${msg})`;
    log(`[RuntimeError] ${m} - ${defaultStack(new Error())}`);
    throw new Error(m);
  }
}

/**
 * @see https://stackoverflow.com/a/37837872
 */
const gSourceMapData = new Map();

/** ###########################################################################
 * Use JS injection prevention:
 * Save some functions before User JS has a chance to overwrite them.
 * NOTE: We access many more monkey-patchable functions.
 * ##########################################################################*/

const JSONStringify = JSON.stringify;
const JSONParse = JSON.parse;
const { bind, call } = Function.prototype;
const uncurryThis = bind.bind(call);
const URLPrototypeToString = uncurryThis(URL.prototype.toString);

// RUN-3067
const ArrayPrototypeFilter = uncurryThis(Array.prototype.filter);
const ArrayPrototypeFind = uncurryThis(Array.prototype.find);
const ArrayPrototypeIncludes = uncurryThis(Array.prototype.includes);
const ArrayPrototypeIndexOf = uncurryThis(Array.prototype.indexOf);
const ArrayPrototypeJoin = uncurryThis(Array.prototype.join);
const ArrayPrototypeMap = uncurryThis(Array.prototype.map);
const ArrayPrototypePop = uncurryThis(Array.prototype.pop);
const ArrayPrototypeReverse = uncurryThis(Array.prototype.reverse);
const ArrayPrototypeSlice = uncurryThis(Array.prototype.slice);
const ArrayPrototypeSort = uncurryThis(Array.prototype.sort);
const ArrayPrototypePush = uncurryThis(Array.prototype.push);
const ObjectPrototypeToString = uncurryThis(Object.prototype.toString);
const StringPrototypeSlice = uncurryThis(String.prototype.slice);
const ObjectGetOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
const uncurryGetter = (proto, key) =>
  uncurryThis(ObjectGetOwnPropertyDescriptor(proto, key).get);
const MapPrototypeGet = uncurryThis(Map.prototype.get);
const MapPrototypeSet = uncurryThis(Map.prototype.set);
const MapPrototypeHas = uncurryThis(Map.prototype.has);
const MapPrototypeDelete = uncurryThis(Map.prototype.delete);
const MapPrototypeClear = uncurryThis(Map.prototype.clear);
const MapPrototypeForEach = uncurryThis(Map.prototype.forEach);
const MapPrototypeGetSize = uncurryGetter(Map.prototype, "size");
const SetPrototypeAdd = uncurryThis(Set.prototype.add);
const SetPrototypeHas = uncurryThis(Set.prototype.has);
const SetPrototypeForEach = uncurryThis(Set.prototype.forEach);
const SetPrototypeGetSize = uncurryGetter(Set.prototype, "size");
const SymbolPrototypeToString = uncurryThis(Symbol.prototype.toString);
const ReflectApply = Reflect.apply;
const ObjectAssign = Object.assign;
const ObjectKeys = Object.keys;
const NumberIsNaN = Number.isNaN;
const MathMax = Math.max;
const MathMin = Math.min;

// DOM and CSSOM members, stored so page patches of the web-platform prototypes
// aren't reached. Blink checks a receiver against the interface's per-isolate
// template, so these also work on nodes and windows of same-process iframes.
const WindowGetComputedStyle = uncurryThis(window.getComputedStyle);
const DocumentPrototypeQuerySelector = uncurryThis(Document.prototype.querySelector);
const DocumentPrototypeGetElementsByTagName = uncurryThis(Document.prototype.getElementsByTagName);
const DocumentFragmentPrototypeQuerySelector = uncurryThis(DocumentFragment.prototype.querySelector);
const ElementPrototypeQuerySelector = uncurryThis(Element.prototype.querySelector);
const ElementPrototypeGetBoundingClientRect = uncurryThis(Element.prototype.getBoundingClientRect);
const ElementPrototypeGetClientRects = uncurryThis(Element.prototype.getClientRects);
const CSSStyleDeclarationPrototypeItem = uncurryThis(CSSStyleDeclaration.prototype.item);
const CSSStyleDeclarationPrototypeGetPropertyValue = uncurryThis(CSSStyleDeclaration.prototype.getPropertyValue);
const CSSStyleDeclarationPrototypeGetPropertyPriority = uncurryThis(CSSStyleDeclaration.prototype.getPropertyPriority);
const CSSStyleValueParse = CSSStyleValue.parse;
const CSSTransformComponentPrototypeToMatrix = uncurryThis(CSSTransformComponent.prototype.toMatrix);
const NodeDocumentNode = Node.DOCUMENT_NODE;
const NodePrototypeGetNodeType = uncurryGetter(Node.prototype, "nodeType");
const NodePrototypeGetNodeName = uncurryGetter(Node.prototype, "nodeName");
const NodePrototypeGetNodeValue = uncurryGetter(Node.prototype, "nodeValue");
const NodePrototypeGetIsConnected = uncurryGetter(Node.prototype, "isConnected");
const NodePrototypeGetParentNode = uncurryGetter(Node.prototype, "parentNode");
const NodePrototypeGetChildNodes = uncurryGetter(Node.prototype, "childNodes");
const NodeListPrototypeGetLength = uncurryGetter(NodeList.prototype, "length");
const NodeListPrototypeItem = uncurryThis(NodeList.prototype.item);
const ElementPrototypeGetTagName = uncurryGetter(Element.prototype, "tagName");
const ElementPrototypeGetAttributes = uncurryGetter(Element.prototype, "attributes");
const ElementPrototypeGetChildren = uncurryGetter(Element.prototype, "children");
const DocumentPrototypeGetChildren = uncurryGetter(Document.prototype, "children");
const NamedNodeMapPrototypeGetLength = uncurryGetter(NamedNodeMap.prototype, "length");
const NamedNodeMapPrototypeItem = uncurryThis(NamedNodeMap.prototype.item);
const AttrPrototypeGetName = uncurryGetter(Attr.prototype, "name");
const AttrPrototypeGetValue = uncurryGetter(Attr.prototype, "value");
const HTMLElementPrototypeGetStyle = uncurryGetter(HTMLElement.prototype, "style");
const SVGElementPrototypeGetStyle = uncurryGetter(SVGElement.prototype, "style");
const HTMLCollectionPrototypeGetLength = uncurryGetter(HTMLCollection.prototype, "length");
const HTMLCollectionPrototypeItem = uncurryThis(HTMLCollection.prototype.item);
const HTMLIFrameElementPrototypeGetContentDocument = uncurryGetter(HTMLIFrameElement.prototype, "contentDocument");
const HTMLIFrameElementPrototypeGetContentWindow = uncurryGetter(HTMLIFrameElement.prototype, "contentWindow");
const DocumentPrototypeGetURL = uncurryGetter(Document.prototype, "URL");
const DocumentPrototypeGetDefaultView = uncurryGetter(Document.prototype, "defaultView");
// Window attributes live on the window object itself, not on a prototype.
const WindowGetDocument = uncurryGetter(window, "document");
const WindowGetParent = uncurryGetter(window, "parent");
const CSSStyleDeclarationPrototypeGetLength = uncurryGetter(CSSStyleDeclaration.prototype, "length");
const CSSTransformValuePrototypeGetLength = uncurryGetter(CSSTransformValue.prototype, "length");
const CSSTransformValuePrototypeGetIs2D = uncurryGetter(CSSTransformValue.prototype, "is2D");
const DOMRectListPrototypeGetLength = uncurryGetter(DOMRectList.prototype, "length");
const DOMRectListPrototypeItem = uncurryThis(DOMRectList.prototype.item);
const DOMRectReadOnlyPrototypeGetLeft = uncurryGetter(DOMRectReadOnly.prototype, "left");
const DOMRectReadOnlyPrototypeGetTop = uncurryGetter(DOMRectReadOnly.prototype, "top");
const DOMRectReadOnlyPrototypeGetRight = uncurryGetter(DOMRectReadOnly.prototype, "right");
const DOMRectReadOnlyPrototypeGetBottom = uncurryGetter(DOMRectReadOnly.prototype, "bottom");

// The element's inline style, for the element interfaces that have one.
function inlineStyleOf(node) {
  try {
    return HTMLElementPrototypeGetStyle(node);
  } catch {}
  try {
    return SVGElementPrototypeGetStyle(node);
  } catch {
    return undefined;
  }
}

// Element and Document each have their own children getter.
function childrenOf(node) {
  try {
    return ElementPrototypeGetChildren(node);
  } catch {}
  try {
    return DocumentPrototypeGetChildren(node);
  } catch {
    return undefined;
  }
}

// A DOMRect as a plain object, read through the stored getters.
function plainRect(rect) {
  return {
    left: DOMRectReadOnlyPrototypeGetLeft(rect),
    top: DOMRectReadOnlyPrototypeGetTop(rect),
    right: DOMRectReadOnlyPrototypeGetRight(rect),
    bottom: DOMRectReadOnlyPrototypeGetBottom(rect),
  };
}

// Index loop over a DOM collection through its stored length and item.
function forEachItem(collection, getLength, item, fn) {
  const length = getLength(collection);
  for (let i = 0; i < length; i++) {
    fn(item(collection, i));
  }
}
const StringPrototypeIndexOf = uncurryThis(String.prototype.indexOf);
const StringPrototypeSubstring = uncurryThis(String.prototype.substring);
const StringPrototypeEndsWith = uncurryThis(String.prototype.endsWith);
const StringPrototypeTrim = uncurryThis(String.prototype.trim);
const RegExpPrototypeExec = uncurryThis(RegExp.prototype.exec);
const FunctionPrototypeToString = uncurryThis(Function.prototype.toString);
const ObjectDefineProperty = Object.defineProperty;
const ObjectHasOwn = Object.hasOwn;
const ObjectCreate = Object.create;
const ObjectSetPrototypeOf = Object.setPrototypeOf;
const ReflectDeleteProperty = Reflect.deleteProperty;
const DOMExceptionPrototypeGetMessage = uncurryGetter(DOMException.prototype, "message");

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
// realm's Error instead. Commands do run page code, so that can happen here;
// the stack then comes out however that realm's hook formats it.
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

// Only our own errors carry a code, as an own property.
function errorCode(err) {
  if (typeof err !== "object" || err === null) return undefined;
  return ObjectHasOwn(err, "code") ? err.code : undefined;
}

// String.prototype.split would also look up Symbol.split through the
// separator's prototype chain.
function splitBy(str, separator) {
  const parts = [];
  let start = 0;
  while (true) {
    const end = StringPrototypeIndexOf(str, separator, start);
    if (end < 0) {
      ArrayPrototypePush(parts, StringPrototypeSlice(str, start));
      return parts;
    }
    ArrayPrototypePush(parts, StringPrototypeSlice(str, start, end));
    start = end + separator.length;
  }
}

// Objects from CDP and the driver only have own data properties, so anything
// found on their prototype chain would be the page's.
function ownProperty(obj, key) {
  return ObjectHasOwn(obj, key) ? obj[key] : undefined;
}

// The listed own properties of `obj` on a prototype-less object, for destructuring.
function ownProperties(obj, keys) {
  const rv = ObjectCreate(null);
  for (let i = 0; i < keys.length; i++) {
    const key = keys[i];
    if (ObjectHasOwn(obj, key)) {
      rv[key] = obj[key];
    }
  }
  return rv;
}

function errorReport(err) {
  const stack = defaultStack(err);
  return {
    is_error: true,
    message: errorMessage(err),
    stack: stack ? splitBy(stack, "\n") : [],
    code: errorCode(err),
  };
}

// for...of, spread and Array.from go through the iterator protocol, whose
// methods the page can patch; these iterate by index or via forEach instead.
function pushAll(array, items) {
  for (let i = 0; i < items.length; i++) {
    ArrayPrototypePush(array, items[i]);
  }
}

function mapKeysArray(map) {
  const keys = [];
  MapPrototypeForEach(map, (value, key) => ArrayPrototypePush(keys, key));
  return keys;
}

function mapValuesArray(map) {
  const values = [];
  MapPrototypeForEach(map, value => ArrayPrototypePush(values, value));
  return values;
}

function describeValueShape(value) {
  let str;
  try {
    str = String(value);
  } catch {
    str = "<String failed>";
  }
  if (str.length > 120) {
    str = StringPrototypeSlice(str, 0, 120) + "...";
  }
  let tag;
  try {
    tag = ObjectPrototypeToString(value);
  } catch {
    tag = "<toString failed>";
  }
  return `${str} (typeof=${typeof value}, ctor=${value?.constructor?.name}, tag=${tag}, length=${value?.length}, blink=${!!fromJsIsBlinkObject(value)})`;
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
    log("[RuntimeError] Failed to process sourcemap url: " + errorMessage(err));
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

///////////////////////////////////////////////////////////////////////////////
// message.js
///////////////////////////////////////////////////////////////////////////////

function initMessages() {
  setCDPMessageCallback(messageCallback);
  setCommandCallback(commandCallback);
  setClearPauseDataCallback(clearPauseDataCallback);
}

let gNextMessageId = 1;

class CdpRequest {
  messageId;
  /**
   * CDP can send three possible types of results:
   *
   * 1. ProtocolError (id?, error: (code, message), data?)
   * @see https://github.com/replayio/chromium-v8/blob/c5e451943a6d87b44374e7a08d44fa92b9a2c93b/third_party/inspector_protocol/crdtp/dispatch.cc#L275
   *
   * 2. Response (id, result) - The response contains the return values defined by CDP.
   * @see https://github.com/replayio/chromium-v8/blob/c5e451943a6d87b44374e7a08d44fa92b9a2c93b/third_party/inspector_protocol/crdtp/dispatch.cc#L348
   *
   * 3. Notification (method, params) - TODO: we are not handling this yet.
   * @see https://github.com/replayio/chromium-v8/blob/c5e451943a6d87b44374e7a08d44fa92b9a2c93b/third_party/inspector_protocol/crdtp/dispatch.cc#L370
   */
  result;

  constructor(messageId) {
    this.messageId = messageId;
  }
}

const gCdpRequestStack = [];
const gEventListeners = new Map();


class CDPMessageError extends Error {
  constructor(message, code) {
    super(`${message} (${code})`);
    this.cdpMessage = message;
    this.code = code;
  }
}

function sendCDPMessage(method, params, contextId) {
  CHECK_ALIVE(`sendCDPMessage ${method}`);

  const id = gNextMessageId++;
  const cdpRequest = new CdpRequest(id);
  ArrayPrototypePush(gCdpRequestStack, cdpRequest);
  const cdpArgs = JSONStringify({ method, params, id });
  try {
    if (contextId === undefined) {
      sendCDPMessageRaw(cdpArgs);
    } else {
      sendCDPMessageRaw(cdpArgs, contextId);
    }
  } catch (err) {
    if (!cdpRequest.result) {
      throw err;
    } else {
      // The CDP request was serviced, followed by a "ghostly" cross-origin
      // (and maybe other?) error:
      // Generally speaking, CDP commands should not throw.
      // If they do, we saw those errors being thrown by previous
      // user JS which happen to still be pending and then get thrown upon CDP
      // result return.
      // E.g.: https://linear.app/replay/issue/RUN-1680#comment-1dfa142b
      log(`[RuntimeError][RUN-1680] sendCDPMessage(${method}) failed: ${errorMessage(err)}`);
    }
  } finally {
    const req = ArrayPrototypePop(gCdpRequestStack);
    assert(req === cdpRequest, "[RuntimeError] CDP request stack corrupted");
  }

  const { result: cdpMessage } = cdpRequest;
  if (!cdpMessage) {
    return undefined;
  }
  const result = ownProperty(cdpMessage, "result");
  if (result) {
    return result;
  }
  const error = ownProperty(cdpMessage, "error");
  if (error) {
    throw new CDPMessageError(ownProperty(error, "message"), ownProperty(error, "code"));
  }
  return undefined;
}

/**	
 * [RUN-3160] We have dependencies on this in the backend, via Target.evaluatePrivileged.	
 * @deprecated Use {@link sendCDPMessage} instead.	
 */	
// eslint-disable-next-line	
const sendMessage = sendCDPMessage;


function addEventListener(method, callback) {
  MapPrototypeSet(gEventListeners, method, callback);
}

// TODO: rename all these CDP-related symbols to also have CDP in the name
function messageCallback(message) {
  try {
    message = JSONParse(message);
    const id = ownProperty(message, "id");
    if (id) {
      const request = gCdpRequestStack[gCdpRequestStack.length - 1];
      assert(id === request.messageId, "CDP request stack corrupted");
      request.result = message;
    } else {
      const listener = MapPrototypeGet(gEventListeners, ownProperty(message, "method"));
      if (listener) {
        listener(ownProperty(message, "params"));
      }
    }
  } catch (e) {
    warning(`JS Message callback exception: ${defaultStack(e) || errorMessage(e)}`);

    return JSONStringify(errorReport(e));
  }
}

///////////////////////////////////////////////////////////////////////////////
// Command Handlers
///////////////////////////////////////////////////////////////////////////////

// Methods for interacting with the record/replay driver.

// Track all current execution contexts so that any scripts that we
// inject via evaluatePrivilegd can know what contexts are available.
const gExecutionContexts = new Map();
const gContextChangeCallbacks = new Set();

const CommandCallbacks = {
  "Graphics.getDevicePixelRatio": Graphics_getDevicePixelRatio,
  "Target.evaluatePrivileged": Target_evaluatePrivileged,
  "Target.getCurrentMessageContents": Target_getCurrentMessageContents,
  "Target.getSourceMapURL": Target_getSourceMapURL,
  "Target.getStepOffsets": Target_getStepOffsets,
  "Target.getCurrentNetworkRequestEvent": Target_getCurrentNetworkRequestEvent,
  "Target.getCurrentNetworkStreamData": Target_getCurrentNetworkStreamData,
  "Target.topFrameLocation": Target_topFrameLocation,
  "Pause.evaluateInFrame": Pause_evaluateInFrame,
  "Pause.evaluateInGlobal": Pause_evaluateInGlobal,
  "Pause.getAllFrames": Pause_getAllFrames,
  "Pause.getExceptionValue": Pause_getExceptionValue,
  "Pause.getObjectPreview": Pause_getObjectPreview,
  "Pause.getObjectProperty": Pause_getObjectProperty,
  "Pause.getScope": Pause_getScope,
  "DOM.forceLayout": DOM_forceLayout,
  "DOM.getDocument": DOM_getDocument,
  "DOM.getAllBoundingClientRects": DOM_getAllBoundingClientRects,
  "DOM.getBoundingClientRect": DOM_getBoundingClientRect,
  "DOM.getBoxModel": DOM_getBoxModel,
  "DOM.getEventListeners": DOM_getEventListeners,
  "DOM.querySelector": DOM_querySelector,
  "DOM.performSearch": DOM_performSearch,
  "CSS.getComputedStyle": CSS_getComputedStyle,
  "CSS.getAppliedRules": CSS_getAppliedRules
};

function CHECK_ALIVE(message) {
  if (!isReplayScriptAlive()) {
    const err = new Error(`ReplayScript UNALIVE - ${message}`);
    err.code = CDPERROR_NOTALIVE;
    if (hasDiverged()) {
      throw err;
    } else {
      // Since we don't know enough about the circumstances here yet,
      // let's not crash an RTP for it.
      warning(defaultStack(err));
    }
  }
}

function getAliveLabel() {
  return isReplayScriptAlive() ? "" : " [UNALIVE]"
}

function executeCommand(method, params) {
  VerboseCommands && log(`[Command ${method}] Handling command, params=${JSONStringify(params)}...`);
  const result = CommandCallbacks[method](params);
  VerboseCommands && log(`[Command ${method}] Handled command, result=${JSONStringify(result)}`);
  return result;
}

function commandCallback(method, params) {
  if (!CommandCallbacks[method]) {
    log(`[RuntimeError][Command ${method}] Missing command callback: ${method}`);
    return {};
  }

  try {
    return executeCommand(method, params);
  } catch (e) {
    log(`[RuntimeError][Command ${method}]${getAliveLabel()} ${defaultStack(e) || errorMessage(e)}`);
    // Pass the error up to V8; it can (for now) decide how to handle itself, whether
    // it should crash or not, etc.  Eventually, the caller of the command should make
    // that decision.
    return errorReport(e);
  }
}

function Target_evaluatePrivileged({ expression }) {
  // Evaluating backend-supplied code in the page is the point of this command.
  const result = eval(expression);
  return { result };
}

const cdpToRrpConsoleLevels = new Map([
  ["info", "info"],
  ["warning", "warning"],
  ["error", "error"],
  ["timeEnd", "timeEnd"]
]);

// Contents of the last console API call. Runtime.consoleAPICalled will be
// emitted before the driver gets the current message contents.
let gLastConsoleAPICall;
function onConsoleAPICall(params) {
  gLastConsoleAPICall = params;
}

function Target_getCurrentMessageContents() {
  // We could be getting the contents of either an error object that was reported
  // to the driver via C++, or a console API call that was reported to the driver
  // via onConsoleAPICall(). We use these two paths because the bookmark
  // associated with thrown exceptions isn't available via the CDP currently.
  const error = getCurrentError();

  if (error) {
    const { message, filename, line, column, scriptId } =
      ownProperties(error, ["message", "filename", "line", "column", "scriptId"]);
    return {
      source: "PageError",
      level: "error",
      text: message,
      url: filename,
      sourceId: scriptId ? String(scriptId) : undefined,
      line,
      column,
    };
  }

  if (!gLastConsoleAPICall) {
    return {
      source: "UnknownMessageError",
      level: "error",
      text: "[RuntimeError] Could not look up message contents"
    };
  }

  // Get the protocol representation of the message arguments.
  const argumentValues = [];
  const args = ownProperty(gLastConsoleAPICall, "args") || [];
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    ArrayPrototypePush(argumentValues, buildRrpObjectFromCdpObject(arg));
  }

  const level = MapPrototypeGet(cdpToRrpConsoleLevels, ownProperty(gLastConsoleAPICall, "type")) || "info";

  let url, sourceId, line, column;
  const stackTrace = ownProperty(gLastConsoleAPICall, "stackTrace");
  if (stackTrace) {
    const frame = ownProperty(stackTrace, "callFrames")[0];
    if (frame) {
      url = ownProperty(frame, "url");
      sourceId = ownProperty(frame, "scriptId");
      line = ownProperty(frame, "lineNumber");
      column = ownProperty(frame, "columnNumber");
    }
  }

  return {
    source: "ConsoleAPI",
    level,
    text: "",
    url,
    sourceId,
    line,
    column,
    argumentValues,
  };
}

addNewScriptHandler((scriptId, sourceURL, relativeSourceMapURL) => {
  if (!relativeSourceMapURL)
    return;

  const urls = getSourceMapURLs(sourceURL, relativeSourceMapURL);
  if (!urls)
    return;

  const { sourceMapURL, sourceMapBaseURL } = urls;
  MapPrototypeSet(gSourceMapData, scriptId, {
    url: sourceMapURL,
    baseUrl: sourceMapBaseURL
  });
}, /* disallowEvents */ true);

function Target_getSourceMapURL({ sourceId }) {
  return MapPrototypeGet(gSourceMapData, sourceId) || {};
}

function Target_getStepOffsets() {
  // CDP does not distinguish between steps and breakpoints.
  return {};
}

function Target_getCurrentNetworkRequestEvent() {
  try {
    const obj = JSONParse(getCurrentNetworkRequestEvent());
    return { data: obj };
  } catch (e) {
    warning(`JS Target.getCurrentNetworkRequestEvent exception: ${e}`);
  }
}

function Target_getCurrentNetworkStreamData(params) {
  const data = getCurrentNetworkStreamData(params);
  if (data) {
    return { data };
  } else {
    warning(`JS Target.getCurrentNetworkStreamData returned no data.`);
  }
}

function Target_topFrameLocation() {
  try {
    if (!isReplayScriptAlive()) {
      return {};
    }
    const rv = sendCDPMessage("Debugger.getTopFrameLocation");
    const location = rv ? ownProperty(rv, "location") : undefined;
    if (!location) {
      return {};
    }
    return { location: createProtocolLocation(location)[0] };
  } catch (e) {
    if (e instanceof CDPMessageError) {
      // No available context group; this can happen, so just return nothing.
      if (e.code == CDPERROR_MISSINGCONTEXT) {
        warning(`[RUN-2600] JS Target_topFrameLocation has no context.`);
        return {};
      }
    }
    throw e;
  }
}

/**
 * Get the raw call frames on the stack, eliding ones in scripts we are ignoring.
 * @return {{ callFrames: CDP.Debugger.CallFrame[] }}
 *
 * @see https://chromedevtools.github.io/devtools-protocol/v8/Debugger/#type-CallFrame
 * @see https://github.com/replayio/chromium-v8/blob/37d50784b68747e7b2d5ebc16305cb9b3227741a/src/inspector/v8-debugger-agent-impl.cc#L1412
 */
function getStackFrames() {
  // NOTE: this is a custom command we added in `src/inspector/v8-debugger-agent-impl.cc`
  try {
    if (!isReplayScriptAlive()) {
      return [];
    }
    const rv = sendCDPMessage("Debugger.getCallFrames", {
      objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP
    });
    return ownProperty(rv, "callFrames");
  } catch (e) {
    if (e instanceof CDPMessageError) {
      // No available context group; this can happen, so just return nothing.
      if (e.code == CDPERROR_MISSINGCONTEXT) {
        warning(`[RUN-2600] JS getStackFrames has no context.`);
        return [];
      }
    }
    throw e;
  }
}


// Build a protocol Result object from a result/exceptionDetails CDP rval.
function buildRrpObjectResult(cdpReturnValue) {
  const rrpResult = { data: {} };
  if (cdpReturnValue) {
    const { result: cdpResult, exceptionDetails } =
      ownProperties(cdpReturnValue, ["result", "exceptionDetails"]);
    if (exceptionDetails) {
      /**
       * @see https://chromedevtools.github.io/devtools-protocol/tot/Runtime/#type-ExceptionDetails
       */
      const exception = ownProperty(exceptionDetails, "exception");
      const rrpObject = exception ?
        buildRrpObjectFromCdpObject(exception) :
        registerPlainObject({ message: ownProperty(exceptionDetails, "text") })
      rrpResult.exception = rrpObject;
    } else if (cdpResult) {
      // cdpResult is the actual result RemoteObject.
      const rrpObject = buildRrpObjectFromCdpObject(cdpResult);
      rrpResult.returned = rrpObject;
    }
  } else {
    // Sometimes things go wrong.
    // E.g. sometimes we get "Cannot find default execution context (-32000) when executing" sendCDPMessage
    // from Pause_evaluateIn*.
    log(`[RuntimeError] buildRrpObjectResult called without cdpReturnValue ()`);
    rrpResult.failed = true;
  }
  return { result: rrpResult };
}


function handleEvalError(err) {
  // RUN-2042 workaround: This fails a lot due to evals on frames in contexts
  // that have been destroyed. We want to fix this if we know that this is
  // high-impact.
  log(`[RuntimeError] in eval: ${defaultStack(err) || errorMessage(err)}`);
  return {
    failed: true
  };
}

let gCurrentEvaluateFrame;

/**
 * This queries all frames and returns the frame of given index.
 * @param {number} frameIndex
 */
function getFrameByIndex(frameIndex) {
  const frames = getStackFrames();
  assert(frameIndex >= 0 && frameIndex < frames.length, `Invalid frame index: ${frameIndex}`);
  return frames[frameIndex];
}

function getFrameByLocation(cdpLocation) {
  const frames = getStackFrames();
  return ArrayPrototypeFind(
    frames,
    f => JSONStringify(ownProperty(f, "location")) == JSONStringify(cdpLocation)
  );
}

/**
 * Returns the frame that `Pause.evaluateInFrame` was called on or undefined,
 * if not in the context of a `Pause.evaluateInFrame` call.
 */
function getCurrentEvaluateFrame() {
  return gCurrentEvaluateFrame;
}

function Pause_evaluateInFrame({ frameId: frameIndexStr, expression }) {
  const frameIndex = +frameIndexStr;
  const frame = getFrameByIndex(frameIndex);
  gCurrentEvaluateFrame = frame;
  let rv;
  try {
    onBeforeEval();
    rv = doEvaluation();
    return buildEvalResult(rv);
  } catch (err) {
    return handleEvalError(err);
  } finally {
    gCurrentEvaluateFrame = undefined;
  }

  function doEvaluation() {
    // In order to do the evaluation in the right frame, the same number of
    // frames need to be on V8's stack when we do the evaluation as when we got
    // the stack frames in the first place. The debugger agent extracts a frame
    // index from the ID it is given and uses that to walk the stack to the
    // frame where it will do the evaluation (see DebugStackTraceIterator).
    return sendCDPMessage(
      "Debugger.evaluateOnCallFrame",
      {
        callFrameId: ownProperty(frame, "callFrameId"),
        expression,
        objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP
      },
      ownProperty(frame, "contextId")
    );
  }
}

function Pause_evaluateInGlobal({ expression }) {
  let rv;
  try {
    onBeforeEval();
    rv = sendCDPMessage(
      "Runtime.evaluate",
      {
        expression,
        objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP
      }
    );
  } catch (err) {
    return handleEvalError(err);
  }
  return buildEvalResult(rv);
}

function onBeforeEval() {
  onReplayApiReset();
}

function buildEvalResult(cdpResult) {
  const exceptionDetails = cdpResult ? ownProperty(cdpResult, "exceptionDetails") : undefined;
  if (usedReplayApi && exceptionDetails) {
    // Emit warning if an eval that used the Replay API throws.
    const exception = ownProperty(exceptionDetails, "exception");
    const cdpException = (exception && ownProperty(exception, "description")) || exceptionDetails;
    warning(`REPLAY_API_EVAL_ERROR ${JSONStringify(cdpException)}`);
  }
  return buildRrpObjectResult(cdpResult);
}

function Pause_getAllFrames() {
  const frames = ArrayPrototypeMap(getStackFrames(), (frame, index) => {
    // Use our own IDs for frames.
    const id = String(index++);
    const topmost = id == 0;
    return createProtocolFrame(id, frame, topmost);
  });
  return {
    frames: ArrayPrototypeMap(frames, f => f.frameId),
    data: { frames },
  };
}

function Pause_getExceptionValue() {
  const rv = sendCDPMessage("Debugger.getPendingException", {
    objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP
  });
  const exception = ownProperty(rv, "exception");
  return { exception: exception ? buildRrpObjectFromCdpObject(exception) : undefined, data: {} };
}

function Pause_getObjectPreview({ object, level = "full", pageSizeForTesting = 0 }) {
  const objectData = createPauseObject(object, level, pageSizeForTesting);
  return { data: { objects: [objectData] } };
}

function Pause_getObjectProperty({ object, name }) {
  const cdpObj = getCdpObjectByRrpId(object);
  const rv = sendCDPMessage(
    "Runtime.callFunctionOn",
    {
      functionDeclaration: `function() { return this["${name}"] }`,
      objectId: ownProperty(cdpObj, "objectId"),
      objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP
    }
  );
  return buildRrpObjectResult(rv);
}

function Pause_getScope({ scope }) {
  const scopeData = createRrpScope(scope);
  return { data: { scopes: [scopeData] } };
}

function Graphics_getDevicePixelRatio() {
  return { ratio: fromJsGetDevicePixelRatio() || 0 };
}


///////////////////////////////////////////////////////////////////////////////
// Utilities
///////////////////////////////////////////////////////////////////////////////

function isPrototype(x) {
  // Note: This can invoke getters or proxy hooks on page objects,
  // so we watch for exceptions being thrown.
  try {
    return x === x?.constructor?.prototype;
  } catch (e) {
    return false;
  }
}

/**
 * Check whether given object `x` is a native object.
 */
function isBlinkObject(x) {
  return fromJsIsBlinkObject(x);
}




///////////////////////////////////////////////////////////////////////////////
// object.js
// Manage association between remote objects and protocol object IDs.
///////////////////////////////////////////////////////////////////////////////


/**
 * This is mostly standard CDP `RemoteObject`s.
 * In some cases, CDP decided to have non-standard objects with
 * a separate id space (e.g. `CSSStylesheet`). We do not store those.
 *
 * @type {Map<string, CDP.Runtime.RemoteObject>}
 */
const gCdpObjectsByRrpId = new Map();

/**
 * @type {Map<string, string>}
 */
const gRrpIdByCdpId = new Map();
/**
 * @type {Map<Object, string>}
 */
const gRrpIdByPlainObject = new Map();
/**
 * @type {Map<string, Object>}
 */
const gPlainObjectByRrpId = new Map();

/**
 * Some preview objects are best constructed at an earlier time and then cached for
 * later use in this map.
 * @type {Map<string, Object>}
 */
const gObjectPreviewByRrpId = new Map();

let gLastRrpId = 0;

// Map protocol ObjectId => Debugger.Scope
// TODO: gCdpScopesByRrpId can probably be removed (use gCdpObjectsByRrpId instead)
const gCdpScopesByRrpId = new Map();

// cheap cache for boundingClientRects
const gLastBoundingClientRectsByNodeRrpId = new Map();

/**
 * @type {Map<>}
 */
const gCssRulesByNodeRrpId = new Map();

function clearPauseDataCallback() {
  try {
    MapPrototypeClear(gCdpObjectsByRrpId);
    MapPrototypeClear(gRrpIdByCdpId);
    MapPrototypeClear(gRrpIdByPlainObject);
    MapPrototypeClear(gPlainObjectByRrpId);
    MapPrototypeClear(gObjectPreviewByRrpId);
    MapPrototypeClear(gCdpScopesByRrpId);
    MapPrototypeClear(gLastBoundingClientRectsByNodeRrpId);
    MapPrototypeClear(gCssRulesByNodeRrpId);
    gLastRrpId = 0;

    if (!isReplayScriptAlive()) {
      return;
    }

    // RUN-1832
    sendCDPMessage("Runtime.releaseObjectGroup", {
      objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP,
    });
  } catch (e) {
    if (e instanceof CDPMessageError && e.code == CDPERROR_MISSINGCONTEXT) {
      return;
    }
    warning(`JS clearPauseDataCallback exception: ${e}`);
  }
}

/**
 * Creates and returns a new `CDP.RemoteObject` for given JS object.
 *
 * @return {CDP.Runtime.RemoteObject}
 * @see https://chromedevtools.github.io/devtools-protocol/tot/Runtime/#type-RemoteObject
 */
function makeDebuggeeValue(plainValue) {
  const remoteObject = fromJsMakeDebuggeeValue(plainValue);
  return remoteObject;
}

function createRrpValueRaw(plainValue) {
  const cdpObject = makeDebuggeeValue(plainValue);
  return buildRrpObjectFromCdpObject(cdpObject);
}

/**
 * @param {Object} plainObject
 * @return {number}
 */
function registerPlainObject(plainObject) {
  assert(isObject(plainObject),
    `value is not an object: ${typeofMaybeNull(plainObject)}`);
  let rrpId = MapPrototypeGet(gRrpIdByPlainObject, plainObject);
  if (!rrpId) {
    // → ask V8InspectorSession to wrap plainObject (gets CDP.Runtime.RemoteObject)
    const cdpObject = makeDebuggeeValue(plainObject);
    if (cdpObject) {
      rrpId = registerCdpObject(cdpObject);
      MapPrototypeSet(gRrpIdByPlainObject, plainObject, rrpId);
      MapPrototypeSet(gPlainObjectByRrpId, rrpId, plainObject);
    }
  }
  return rrpId;
}

function getPlainObjectByCdpId(cdpId) {
  const rrpId = MapPrototypeGet(gRrpIdByCdpId, cdpId);
  assert(rrpId);
  return getPlainObjectByRrpId(rrpId);
}

/**
 * @param {number} rrpId
 * @return {Object}
 */
function getPlainObjectByRrpId(rrpId) {
  rrpId += '';
  let plainObject = MapPrototypeGet(gPlainObjectByRrpId, rrpId);
  if (!plainObject) {
    // (if this was a ref type, registration should already have been handled in `registerCdpObject` ↓)
    // → ask V8InspectorSession to unwrap cdpObject (gets plainObject)
    const cdpObject = getCdpObjectByRrpId(rrpId);
    // → NOTE if we have an rrpId, it means, we already should have registered the cdpObject
    assert(cdpObject);
    const cdpId = ownProperty(cdpObject, "objectId");
    plainObject = fromJsGetObjectByCdpId(cdpId);
    MapPrototypeSet(gRrpIdByPlainObject, plainObject, rrpId);
    MapPrototypeSet(gPlainObjectByRrpId, rrpId, plainObject);
  }
  return plainObject;
}

/**
 * @param {CDP.Runtime.RemoteObject}
 * @return {number} rrpId
 */
function registerCdpObject(cdpObject) {
  const cdpId = ownProperty(cdpObject, "objectId");
  assert(cdpId);

  let rrpId = MapPrototypeGet(gRrpIdByCdpId, cdpId);
  if (rrpId) {
    return rrpId;
  }

  let plainObject;
  if (isCdpRefType(cdpObject)) {
    // NOTE: the same object might generate multiple cdpIds
    plainObject = fromJsGetObjectByCdpId(cdpId);
    if (plainObject) {
      rrpId = MapPrototypeGet(gRrpIdByPlainObject, plainObject);
    }
  }

  return registerNewRrpObject(rrpId, cdpObject, null, plainObject);
}


/**
 *
 * @return {CDP.Runtime.RemoteObject | Object}
 */
function getCdpObjectByRrpId(rrpId) {
  const cdpObject = MapPrototypeGet(gCdpObjectsByRrpId, rrpId);
  if (!cdpObject) {
    throw new Error(`getCdpObjectByRrpId failed - rrpId not found: ${JSONStringify(rrpId)}`);
  }
  return cdpObject;
}

/**
 * Edge case: CDP calls produce custom objects that do NOT have an `objectId`.
 * Sometimes, they have their own id which refers back to some native plainObject
 *   (e.g. `CSSStylesheet`).
 * Sometimes they do not map to a native plainObject (e.g. `CSSRule`).
 * For such a CDP object, we only store its RRP preview extra and, for now, discard
 * its CDP representation.
 *
 *
 * @param {object} rrpObjectPreview Used in `getObjectPreview`.
 * @return {number} rrpId
 *
 * @see https://static.replay.io/protocol/tot/Pause/#type-ObjectPreview
 */
function registerRrpPreview(rrpObjectPreview, plainObject) {
  let rrpId;
  if (plainObject) {
    rrpId = MapPrototypeGet(gRrpIdByPlainObject, plainObject);
  }

  // NOTE: we built a custom "preview object" without a cdpObject, and sometimes without a plainObject
  const cdpObject = null;
  return registerNewRrpObject(rrpId, cdpObject, rrpObjectPreview, plainObject);
}

/**
 * Generates `rrpId`, if it does not have one yet.
 * Associates `rrpId` with its related data.
 */
function registerNewRrpObject(rrpId, cdpObject, rrpObjectPreview, plainObject) {
  // new RrpId
  const existingRrpId = rrpId;
  rrpId ||= ++gLastRrpId + '';  // coerce to string
  if (cdpObject) {
    // CDP.Runtime.RemoteObject
    const cdpId = ownProperty(cdpObject, "objectId");
    assert(cdpId);
    registerRrpCpdId(rrpId, cdpId, cdpObject);
  }
  if (rrpObjectPreview) {
    // preview objects, already built from specialized CDP objects
    MapPrototypeSet(gObjectPreviewByRrpId, rrpId, rrpObjectPreview);
    rrpObjectPreview.objectId = rrpId; // set `objectId`
  }
  if (plainObject && !existingRrpId) {
    MapPrototypeSet(gRrpIdByPlainObject, plainObject, rrpId);
    MapPrototypeSet(gPlainObjectByRrpId, rrpId, plainObject);
  }

  return rrpId;
}

function registerRrpCpdId(rrpId, cdpId, cdpObject = null) {
  MapPrototypeSet(gRrpIdByCdpId, cdpId, rrpId);
  if (cdpObject) {
    MapPrototypeSet(gCdpObjectsByRrpId, rrpId, cdpObject);
  }
}

/**
 * "Universal `arguments`" for any frame:
 * `arguments` are available by default in many function frames. However,
 * arrow functions do not have `arguments` available.
 * This function provides `arguments` for any type of frame.
 * @param {number | object | undefined} [frameOrFrameIndex] Optional frame argument. If none provided, pick the frame that the current Pause.evaluateInFrame call was requested for.
 * @see https://linear.app/replay/issue/RUN-1061#comment-fc1c3ee4
 * @see https://linear.app/replay/issue/RUN-2969/arrow-functions-the-arguments-keyword-and-chromium-vs-gecko#comment-989283b0
 */
function getFrameArgumentsArray(frameOrFrameIndex) {
  let frame;
  if (!frameOrFrameIndex) {
    if (!gCurrentEvaluateFrame) {
      throw new Error(`getFrameArgumentsArray must be called with a frame` +
        `object, frameIndex, or, if none provided, must be called from within ` +
        `the context of a Pause.evaluateInFrame call.`);
    }
    // Get new frame instance, since the stack might have changed and V8 uses
    // frame index for look up.
    frame = getFrameByLocation(ownProperty(gCurrentEvaluateFrame, "location"))
    if (!frame) {
      throw new Error(
        `getFrameArgumentsArray was called from within Pause.evaluateInFrame ` +
        `but the frame is not on stack anymore: ${JSONStringify(ArrayPrototypeMap(getStackFrames(), f => ownProperty(f, "location")))}`);
    }
  } else if (typeof frameOrFrameIndex === "number") {
    frame = getFrameByIndex(frameOrFrameIndex);
  } else if (isObject(frameOrFrameIndex) && ownProperty(frameOrFrameIndex, "callFrameId")) {
    frame = frameOrFrameIndex;
  }
  const frameId = ownProperty(frame, "callFrameId");
  const args = fromJsGetArgumentsInFrame(frameId);
  return args ? ArrayPrototypeSlice(args) : [];
}



// Strings longer than this will be truncated when creating protocol values.
// TODO This limit creates problems when we try to evaluate large strings in routines,
// such as stringifying a large object/array (like 2000+ unmounted fiber IDs).
// The RDT routine works around this by splitting the string into chunks, but
// we should find a better long-term solution (like bypassing the limit for evals).
const MaxStringLength = 10000;

const cdpRefTypes = ['object', 'function'];
function isCdpRefType(cdpObject) {
  return ArrayPrototypeIncludes(cdpRefTypes, ownProperty(cdpObject, "type"));
}


/**
 *
 * @return {RRP.Pause.Object}
 */
function buildRrpObjectFromCdpObject(cdpObject) {
  if (!cdpObject) {
    return {};
  }
  const { type, value, unserializableValue } =
    ownProperties(cdpObject, ["type", "value", "unserializableValue"]);
  switch (type) {
    case "undefined":
      return {};
    case "string":
    case "number":
    case "boolean":
      if (unserializableValue) {
        assert(type == "number");
        return { unserializableNumber: unserializableValue };
      }
      if (typeof value == "string" && value.length > MaxStringLength) {
        return { value: StringPrototypeSubstring(value, 0, MaxStringLength) + "…" };
      }
      return { value };
    case "bigint": {
      const str = unserializableValue;
      assert(str);
      return { bigint: StringPrototypeSubstring(str, 0, str.length - 1) };
    }
    case "object":
    case "function": {
      if (!ownProperty(cdpObject, "objectId")) {    // TODO: how can this happen?
        return { value: null };
      }

      const rrpId = registerCdpObject(cdpObject);
      return { object: rrpId };
    }
    case "symbol":
      return { symbol: ownProperty(cdpObject, "description") };
    default:
      log(`[RuntimeError] invalid CDP type: ${JSONStringify(cdpObject)}`);
      return { unavailable: true };
  }
}


/**
 *
 * @param {CDP.Runtime.Scope} scope
 */
function registerCdpScope(scope) {
  const rrpId = registerCdpObject(ownProperty(scope, "object"));
  MapPrototypeSet(gCdpScopesByRrpId, rrpId, scope);
  return rrpId;
}

function getCdpScopeByRrpId(rrpScopeId) {
  const scope = MapPrototypeGet(gCdpScopesByRrpId, rrpScopeId);
  assert(scope);
  return scope;
}

function getBlinkNodeIdByRrpId(nodeRrpId) {
  const cdpObject = getCdpObjectByRrpId(nodeRrpId);
  const nodeId = fromJsGetNodeIdByCpdId(ownProperty(cdpObject, "objectId"));
  // Note: Don't generate assert message if assert did not fail.
  assert(nodeId, !nodeId && `${nodeRrpId}: ${JSONStringify(cdpObject)}`);
  return nodeId;
}

///////////////////////////////////////////////////////////////////////////////
// preview.js
///////////////////////////////////////////////////////////////////////////////

// Logic for creating object previews for the record/replay protocol.

function isCdpObjectProxy(cdpObj) {
  return ownProperty(cdpObj, "subtype") === "proxy";
}

function isCdpObjectPromise(cdpObj) {
  return ownProperty(cdpObj, "subtype") === "promise";
}

/**
 * @return {RRP.Pause.Object}
 * @see https://static.replay.io/protocol/tot/Pause/#type-Object
 */
function createPauseObject(rrpId, level, pageSizeForTesting) {
  rrpId = rrpId + ""; // Must be a string.
  const existingPreview = MapPrototypeGet(gObjectPreviewByRrpId, rrpId);
  if (existingPreview) {
    return existingPreview;
  }

  const cdpObj = getCdpObjectByRrpId(rrpId);
  // NOTE: `subtype` is not reliably available, due to a divergence check in V8 → `value-mirror.cc`
  const className = isCdpObjectProxy(cdpObj) ? "Proxy" : (ownProperty(cdpObj, "className") || "Function");

  // NOTE: `persistentId` is added in V8 → `injected-script.cc`
  const persistentId = ownProperty(cdpObj, "persistentId");
  let preview;
  if (level != "none") {
    preview = new ProtocolObjectPreview(rrpId, cdpObj, level, pageSizeForTesting).fill();
  }

  return { objectId: rrpId, persistentId, className, preview };
}

// Return whether an object should be ignored when generating previews.
function isObjectBlacklisted(cdpObj) {
  // Accessing Storage object properties can cause hangs when trying to
  // communicate with the non-existent parent process.
  if (ownProperty(cdpObj, "className") == "Storage") {
    return true;
  }

  // Don't inspect scripted proxies, as we could end up calling into script.
  if (isCdpObjectProxy(cdpObj)) {
    return true;
  }

  return false;
}

// Return whether an object's property should be ignored when generating previews.
function isObjectPropertyBlacklisted(cdpObj, name) {
  if (isObjectBlacklisted(cdpObj)) {
    return true;
  }
  switch (name) {
    case "__proto__":
      // Accessing __proto__ doesn't cause problems, but is redundant with the
      // prototype reference included in the preview directly.
      return true;
  }
  return false;
}

// Target limit for the number of items (properties etc.) to include in object
// previews before overflowing.
const MaxItems = {
  "noProperties": 0,

  // Note: this is higher than on gecko-dev because typed arrays don't render
  // properly in the devtools currently unless we include a minimum number of
  // properties. This would be nice to fix.
  "canOverflow": 10,

  "full": 1000,
};

function ProtocolObjectPreview(rrpId, obj, level, pageSizeForTesting) {
  this.rrpId = rrpId;
  this.cdpObj = obj;
  this.level = level;
  this.pageSizeForTesting = pageSizeForTesting;
  this.overflow = false;
  this.numItems = 0;
  this.extra = {};
}

ProtocolObjectPreview.prototype = {
  get raw() {
    return this.plainObject;
  },

  get plainObject() {
    if (!this._plainObject) {
      this._plainObject = getPlainObjectByCdpId(ownProperty(this.cdpObj, "objectId"));
    }
    return this._plainObject;
  },

  startAddItem(force) {
    if (!force) {
      if (this.hasReachedItemLimit) {
        this.overflow = true;
        return false;
      }
      this.numItems++;
    }
    return true;
  },

  checkAddProperty(ownerCdpObject, name) {
    if (isObjectPropertyBlacklisted(ownerCdpObject, name)) {
      return false;
    }
    if (this.getterValues && MapPrototypeHas(this.getterValues, name)) {
      return false;
    }
    return true;
  },

  addProperty(ownerCdpObject, rrpProp, force) {
    if (this.checkAddProperty(ownerCdpObject, rrpProp.name)) {
      this.addPropertyUnchecked(rrpProp, force);
    }
  },

  addPropertyUnchecked(rrpProp, force) {
    if (!this.startAddItem(force)) {
      return;
    }
    if (!this.properties) {
      this.properties = [];
    }
    ArrayPrototypePush(this.properties, rrpProp);
  },

  addGetterValue(propKey, ownerCdpObject, force = false) {
    if (isObjectPropertyBlacklisted(ownerCdpObject, propKey)) {
      return;
    }

    if (!this.getterValues) {
      this.getterValues = new Map();
    }
    if (MapPrototypeHas(this.getterValues, propKey)) {
      return;
    }

    const rrpValue = evalPropRrpNotNull(this.raw, propKey);
    if (rrpValue) {
      this.setGetterValueUnchecked(propKey, rrpValue, force);
    }
  },

  addEvalMethodValue(propKey) {
    if (!this.getterValues) {
      this.getterValues = new Map();
    }
    if (MapPrototypeHas(this.getterValues, propKey)) {
      return;
    }

    // Calling the previewed object's own method is intended here.
    const plainValue = ReflectApply(this.raw[propKey], this.raw, []);
    const rrpValue = createRrpValueRaw(plainValue);
    if (rrpValue) {
      this.setGetterValueUnchecked(propKey, rrpValue, /* force */ true);
    }
    return plainValue;
  },

  setGetterValueUnchecked(key, valueObject, force = true) {
    if (!this.startAddItem(force)) {
      return;
    }
    if (!this.getterValues) {
      this.getterValues = new Map();
    }
    MapPrototypeSet(this.getterValues, key, { name: key, ...valueObject });
  },


  addContainerEntry(entry) {
    if (!this.startAddItem()) {
      return;
    }
    if (!this.containerEntries) {
      this.containerEntries = [];
    }
    ArrayPrototypePush(this.containerEntries, entry);
  },

  get unlimitedItems() {
    // Ignore prop limits of native objects.
    // (Because that is how we do it in gecko.)
    return isBlinkObject(this.raw, this.cdpObj);
  },

  get nRequestedItems() {
    return MaxItems[this.level] || 10;
  },

  get hasReachedItemLimit() {
    if (this.unlimitedItems) {
      return false;
    }
    return this.numItems >= this.nRequestedItems;
  },

  /**
   * Limit the amount of props we get back from CDP Runtime.getProperties.
   * @see https://linear.app/replay/issue/RUN-1315/very-bad-command-performance-getallframes-wandb#comment-f8f54931
   */
  get pageSize() {
    if (this.pageSizeForTesting) {
      return this.pageSizeForTesting;
    }
    if (this.unlimitedItems) {
      // 0 == no limit.  we'll only do a single fetch in the loop in fill().
      return 0;
    }

    return this.nRequestedItems;
  },

  /**
   * Ignore certain prototype props.
   */
  shouldAddProp(cdpProp) {
    if (isBlinkObject(this.raw, this.cdpObj)) {
      // for native objects we've explicitly asked for more than just ownProperties,
      // so we further filter them here.
      const { isOwn, configurable, enumerable } =
        ownProperties(cdpProp, ["isOwn", "configurable", "enumerable"]);
      if (!isOwn && !(configurable && enumerable)) {
        // the property is both not our own, and it's also not on a prototype and configurable + enumerable.
        // XXX(toshok) do we really want to exclude non-configurable props?
        return false;
      }
    }

    return this.checkAddProperty(this.cdpObj, ownProperty(cdpProp, "name"));
  },

  fill() {
    // Data returned from V8 debugger.
    let cdpProperties;

    if (this.pageSizeForTesting && !(this.pageSizeForTesting > 0)) {
      throw new Error("invalid pageSizeForTesting: " + this.pageSizeForTesting);
    }

    // Names of properties + getters.
    const foundProps = new Set();

    if (this.level === 'noProperties') {
      cdpProperties = { result: [] };
    } else {
      const propertiesToFetch = this.pageSize;

      // WARNING: we manage possible divergences caused by `Runtime.getProperties` evaluating native getter
      //    in V8's |doesAttributeHaveObservableSideEffectOnGet|.
      //    see: https://github.com/replayio/chromium-v8/pull/115/files#diff-72ee0a91d32565577bd78ed94b034ae3b4bf51676c5d42165e9363cad18dccf9R1328
      try {
        cdpProperties = sendCDPMessage("Runtime.getProperties", {
          objectId: ownProperty(this.cdpObj, "objectId"),
          ownProperties: !isBlinkObject(this.raw, this.cdpObj),
          generatePreview: false,
          pageIndex: 0, // Warning: NYI
          pageSize: this.unlimitedItems ? 0 : (propertiesToFetch + 1), // +1 so we can detect overflow
          objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP
        });
      } catch (e) {
        // Preview is best-effort: any CDP failure → empty props (match Chrome expand).
        // Rethrow would become commandCallback is_error → Command.cpp Die.
        if (e instanceof CDPMessageError) {
          warning(
            `[crash-0050] ProtocolObjectPreview.fill CDP error ${errorCode(e)}: ${e.cdpMessage || errorMessage(e)}`,
          );
          cdpProperties = { result: [] };
        } else {
          throw e;
        }
      }

      const cdpProps = ownProperty(cdpProperties, "result");
      if (!cdpProps) {
        return {
          prototypeId: undefined
        };
      }

      /**
       * @see https://chromedevtools.github.io/devtools-protocol/tot/Runtime/#type-PropertyDescriptor
       */
      for (let i = 0; i < cdpProps.length; ++i) {
        const cdpProp = cdpProps[i];
        const propKey = ownProperty(cdpProp, "name");
        if (propKey === "__proto__" || SetPrototypeHas(foundProps, propKey)) {
          continue;
        }
        if (this.shouldAddProp(cdpProp)) {
          SetPrototypeAdd(foundProps, propKey);
        }
      }
    }
    
    const cdpProps = ownProperty(cdpProperties, "result");
    for (let i = 0; i < cdpProps.length; i++) {
      const cdpProp = cdpProps[i];
      const propKey = ownProperty(cdpProp, "name");
      if (!SetPrototypeHas(foundProps, propKey)) {
        continue;
      }
      const rrpProp = createRrpPropertyDescriptor(cdpProp);
      const force = false;
      this.addPropertyUnchecked(rrpProp, force);
    }

    /**
     * Explanation:The following logic depends on more `cdpProperties` but
     * is not affected by above `pageSize`:
     * 
     * 1. Inherent props of built-ins, such as Error.stack or Array.length are
     *    always added unconditionally.
     * 2. {Weak,}{Set,Map} is a container type whose contents are not in
     *    `properties`, but rather require a separate query that only returns
     *    actual container contents, thereby not requiring above loop.
     */

    // Add builtin-specific data.
    if (!isPrototype(this.raw)) { // Ignore prototype itself.
      const previewers = ownProperty(CustomPreviewers, ownProperty(this.cdpObj, "className"));
      if (previewers) {
        for (let i = 0; i < previewers.length; i++) {
          const entry = previewers[i];
          if (isFunction(entry)) {
            ReflectApply(entry, this, [cdpProperties]);
          } else {
            // entry should be string -> Look it up in results
            const cdpEntry = ArrayPrototypeFind(ownProperty(cdpProperties, "result"), prop => ownProperty(prop, "name") === entry);
            if (cdpEntry) {
              const rrpEntry = buildRrpObjectFromCdpObject(ownProperty(cdpEntry, "value"));
              this.setGetterValueUnchecked(entry, rrpEntry);
            }
          }
        }
      }
    }
    // Add data for blink and other special objects.
    ObjectAssign(this.extra, getExtraObjectPreviewData(this.cdpObj, cdpProperties));
    // Add Prototype data.
    let prototypeCdp = internalPropValue(cdpProperties, '[[Prototype]]');
    let prototypeRrpId;
    if (prototypeCdp) {
      prototypeRrpId = registerCdpObject(prototypeCdp);
    }

    // Produce final PauseData object.
    const result = {
      prototypeId: prototypeRrpId,
      overflow: (this.overflow && this.level != "full") ? true : undefined,
      properties: this.properties,
      getterValues: this.getterValues ? mapValuesArray(this.getterValues) : undefined,
      containerEntries: this.containerEntries,
      ...this.extra,
    };

    return result;
  }
};

function getExtraObjectPreviewData(cdpObject, cdpProperties) {
  const cdpId = ownProperty(cdpObject, "objectId");
  const rrpId = MapPrototypeGet(gRrpIdByCdpId, cdpId);
  assert(rrpId);
  
  if (isCdpObjectProxy(cdpObject)) {
    let targetCdpObj = internalPropValue(cdpProperties, '[[Target]]');
    let handlerCdpObj = internalPropValue(cdpProperties, '[[Handler]]');
    return {
      proxyState: {
        target: buildRrpObjectFromCdpObject(targetCdpObj),
        handler: buildRrpObjectFromCdpObject(handlerCdpObj)
      }
    };
  } else if (isCdpObjectPromise(cdpObject)) {
    let stateCdpObj = internalPropValue(cdpProperties, '[[PromiseState]]');
    let valueCdpObj = internalPropValue(cdpProperties, '[[PromiseResult]]');
    const promiseState = {
      state: ownProperty(stateCdpObj, "value") || undefined
    };
    if (promiseState.state !== "pending") {
      promiseState.value = buildRrpObjectFromCdpObject(valueCdpObj);
    }
    return {
      promiseState
    };
  } else {
    const plainObject = getPlainObjectByRrpId(rrpId);
    if (fromJsIsBlinkNodeObject(plainObject)) {
      return {
        node: previewBlinkNode(plainObject)
      }
    }

    if (fromJsIsBlinkCSSStyleDeclarationObject(plainObject)) {
      return {
        style: previewBlinkStyle(plainObject)
      }
    }
  }
}

function previewBlinkNode(node) {
  let attributes, pseudoType;
  if (fromJsIsBlinkElementObject(node)) {
    attributes = [];
    forEachItem(ElementPrototypeGetAttributes(node), NamedNodeMapPrototypeGetLength, NamedNodeMapPrototypeItem, attr => {
      ArrayPrototypePush(attributes, { name: AttrPrototypeGetName(attr), value: AttrPrototypeGetValue(attr) });
    });
    // TODO: We cannot access pseudo elements using the JS DOM API - https://linear.app/replay/issue/RUN-953/
    // pseudoType = node.localName;
  }

  const nodeType = NodePrototypeGetNodeType(node);
  const nodeName = NodePrototypeGetNodeName(node);

  let style;
  const inlineStyle = inlineStyleOf(node);
  if (inlineStyle) {
    style = registerPlainObject(inlineStyle);
  }

  let parentNode;
  const rawParentNode = NodePrototypeGetParentNode(node);
  const defaultView = nodeType == NodeDocumentNode ? DocumentPrototypeGetDefaultView(node) : undefined;
  const parentWindow = defaultView ? WindowGetParent(defaultView) : undefined;
  const parentDocument = parentWindow && parentWindow != defaultView ? WindowGetDocument(parentWindow) : undefined;
  if (rawParentNode) {
    parentNode = registerPlainObject(rawParentNode);
  } else if (parentDocument) {
    /**
     * Nested documents use the parent element instead of null.
     *
     * TODO: will need more work here to support multi-CSP iframes
     *   (properly handle `iframe`s and the case where `node.defaultView.parent.document` is missing)
     *   Issue: https://linear.app/replay/issue/RUN-954/dom-feature-support-multi-cspcross-origin-iframes
     */
    const iframes = DocumentPrototypeGetElementsByTagName(parentDocument, "iframe");
    let iframe;
    forEachItem(iframes, HTMLCollectionPrototypeGetLength, HTMLCollectionPrototypeItem, f => {
      if (!iframe && HTMLIFrameElementPrototypeGetContentDocument(f) == node) {
        iframe = f;
      }
    });
    if (iframe) {
      parentNode = registerPlainObject(iframe);
    }
  }

  let documentURL;
  if (nodeType == NodeDocumentNode) {
    documentURL = DocumentPrototypeGetURL(node);
  }

  const rv = {
    nodeType,
    nodeName,
    nodeValue: typeof NodePrototypeGetNodeValue(node) === "string" ? NodePrototypeGetNodeValue(node) : undefined,
    isConnected: NodePrototypeGetIsConnected(node),
    attributes,
    pseudoType,
    style,
    parentNode,
    documentURL,
  };
  

  let childNodes;
  const contentDocument = nodeName == "IFRAME" ? HTMLIFrameElementPrototypeGetContentDocument(node) : undefined;
  const rawChildNodes = NodePrototypeGetChildNodes(node);
  if (contentDocument) {
    // Treat an iframe's content document as one of its child nodes.
    childNodes = [registerPlainObject(contentDocument)];
  } else if (NodeListPrototypeGetLength(rawChildNodes)) {
    childNodes = [];
    forEachItem(rawChildNodes, NodeListPrototypeGetLength, NodeListPrototypeItem, (n) => {
      ArrayPrototypePush(childNodes, registerPlainObject(n));
    });
  }

  if (childNodes) {
    rv.childNodes = childNodes;
  }

  return rv;
}

function previewBlinkStyle(style) {
  // NOTE: this is for inline styles, where there is no parentRule
  let parentRule = undefined;

  const properties = [];
  const styleLength = CSSStyleDeclarationPrototypeGetLength(style);
  for (let i = 0; i < styleLength; i++) {
    const name = CSSStyleDeclarationPrototypeItem(style, i);
    const value = CSSStyleDeclarationPrototypeGetPropertyValue(style, name);
    if (value) {
      const important = CSSStyleDeclarationPrototypeGetPropertyPriority(style, name) == "important" ? true : undefined;
      ArrayPrototypePush(properties, { name, value, important });
    }
  }

  return {
    cssText: style.cssText,
    parentRule,
    properties
  };
}

function getDescriptionCount(description) {
  const match = RegExpPrototypeExec(/\((\d+)\)/, description || "");
  if (match) {
    return +match[1];
  }
  return undefined;
}

function previewArray(_cdpProperties) {
  // TODO: [RUN-2223] Find out why Array.length does not always return a value.
  const length = getDescriptionCount(ownProperty(this.cdpObj, "description"));
  this.setGetterValueUnchecked("length", createRrpValueRaw(length));
}

function previewTypedArray() {
  // simply invoke the native getter
  this.addGetterValue('length', this.cdpObj, /* force */ true);
  this.addGetterValue('byteLength', this.cdpObj, /* force */ true);
  this.addGetterValue('byteOffset', this.cdpObj, /* force */ true);
  this.addGetterValue('buffer', this.cdpObj, /* force */ true);
}

/**
 * Query the internal object of {Weak,}{Set,Map}s that store their
 * containerEntries.
 */
function previewSetMap(cdpProperties) {
  const internal = getInternalProp(cdpProperties, "[[Entries]]");
  const internalValue = internal ? ownProperty(internal, "value") : undefined;
  const internalObjectId = internalValue ? ownProperty(internalValue, "objectId") : undefined;
  if (!internalObjectId) {
    return;
  }

  // Get size from description.
  let size;

  const className = ownProperty(this.cdpObj, "className");
  if (ArrayPrototypeIncludes(["Set", "Map"], className)) {
    // NOTE: For some reason, the internal backing array size is capped to
    // pageSize for Set and Map.
    // This type of inconsistency is possible since *we* added paging to the
    // debugger (RUN-1315), and it might have (albeit small) negative impacts
    // like this.
    // SLN: Simply query the size getter instead.
    size = className === "Map"
      ? MapPrototypeGetSize(this.raw)
      : SetPrototypeGetSize(this.raw);
    const rrpSize = { name: "size", value: size };
    this.addPropertyUnchecked(rrpSize, /* force */ true);
    this.setGetterValueUnchecked(rrpSize.name, rrpSize, /* force */ true);
  } else {
    // Weak{Set,Map}
    size = getDescriptionCount(ownProperty(internalValue, "description"));
  }
  this.extra.containerEntryCount = size;

  const entries = ownProperty(sendCDPMessage("Runtime.getProperties", {
    objectId: internalObjectId,
    ownProperties: true,
    generatePreview: false,
    pageIndex: this.pageIndex,
    pageSize: this.pageSize,
    objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP
  }), "result");

  for (let i = 0; i < entries.length; i++) {
    const entryValue = entries[i] ? ownProperty(entries[i], "value") : undefined;
    if (entryValue && ownProperty(entryValue, "subtype") == "internal#entry") {
      const entryProperties = ownProperty(sendCDPMessage("Runtime.getProperties", {
        objectId: ownProperty(entryValue, "objectId"),
        ownProperties: true,
        generatePreview: false,
        objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP
      }), "result");
      const key = ArrayPrototypeFind(entryProperties, eprop => ownProperty(eprop, "name") == "key");
      const value = ArrayPrototypeFind(entryProperties, eprop => ownProperty(eprop, "name") == "value");
      if (value) {
        this.addContainerEntry({
          key: key ? buildRrpObjectFromCdpObject(ownProperty(key, "value")) : undefined,
          value: buildRrpObjectFromCdpObject(ownProperty(value, "value")),
        });
      }
    }
    if (this.overflow) {
      break;
    }
  }
}

function previewRegExp() {
  this.extra.regexpString = ownProperty(this.cdpObj, "description");
}

function previewDate() {
  const dateTime = Date.parse(ownProperty(this.cdpObj, "description"));
  if (!NumberIsNaN(dateTime)) {
    this.extra.dateTime = dateTime;
  }
}

function previewError() {
  this.setGetterValueUnchecked("name", { value: ownProperty(this.cdpObj, "className") });
}

const ErrorProperties = [
  "message",
  "stack",
  previewError,
];

function getInternalProp(cdpProperties, name) {
  const internalProperties = ownProperty(cdpProperties, "internalProperties");
  return internalProperties
    ? ArrayPrototypeFind(internalProperties, prop => ownProperty(prop, "name") == name)
    : undefined;
}

function internalPropValue(cdpProperties, name) {
  const prop = getInternalProp(cdpProperties, name);
  return prop ? ownProperty(prop, "value") : undefined;
}

function getInternalFunctionLocationProp(cdpProperties) {
  return getInternalProp(cdpProperties, '[[FunctionLocation]]');
}

/**
 * String utility for function parameter parsing.
 */
function substringEndsAt(haystack, needle, i) {
  return StringPrototypeSubstring(haystack, 1 + i - needle.length, 1 + i) === needle;
}

/**
 * @return {boolean} (a + b).endsWith(end)
 */
function stringsEndWith(a, b, end) {
  // NOTE: This can be done more performantly.
  return StringPrototypeEndsWith(a + b, end);
}

const complementaryTokensStart = "({[\"'`";
const complementaryTokensEnd =   ")}]\"'`";

/**
 * [RUN-3146] Extract parameter names from the description.
 * @param {string} s 
 * @return {string[]}
 */
function extractFunctionParameterNames(s) {
  try {
    /** 
     * Function head declaration patterns:
     * P1. modifiers function f(PARAMS) Body
     * P2. modifiers f(PARAMS) Body  // ObjectMethod, ClassMethod
     * P3. modifiers (PARAMS) => ExpressionOrBody
     * P4. modifiers PARAM => ExpressionOrBody
     */

    // All patterns fall into two buckets:
    // A. PARENS: Get the thing within the first opening pair of parentheses.
    // B. SINGLEARROW: Get the single param before =>.

    // Base case: Find pattern A or B, while ignoring comments.
    // Special case 1: Parameter initializers (e.g. `x = f()`).
    //    Commas can be part of initializers, but they would have to be contained
    //    by complementary tokens, any of: ``""''[](){}. 
    //    → Let's simply ignore everything inside of that.
    // Special case 2: Argument destructuring.
    //    -> Don't give the param a name (empty string).
    //    Same parsing logic as case 1.

    /**
     * The function header without (i) comments and without (ii) nested AST nodes.
     */
    let cleanHeader = "";
    /**
     * When in a comment, this is set to the counterpart that we are looking for.
     * @type {string | null}
     */
    let expectedCommentEnd = null;
    let parensStart = -1;
    let insideInitializer = false;
    /**
     * @type {string[]}
     */
    const ignoreStack = [];
    for (let i = 0; i < s.length; ++i) {
      const c = s[i];
      if (expectedCommentEnd) {
        // In a comment.
        if (substringEndsAt(s, expectedCommentEnd, i)) {
          // Comment End: Start new segment from here.
          expectedCommentEnd = null;
        }
        continue;
      }

      // Not in a comment.
      if (StringPrototypeEndsWith(cleanHeader, "/") && (c === "/" || c === "*")) {
        // Comment Start.
        if (c === "*") {
          expectedCommentEnd = "*/";
        } else {
          expectedCommentEnd = "\n";
        }
        // Remove "/" from header:
        cleanHeader = StringPrototypeSlice(cleanHeader, 0, -1);
        continue;
      }

      // Parse everything but comments:

      if (parensStart === -1) {
        // Not in params parentheses.
        if (c === "(") {
          // PARENS: Params start.
          cleanHeader += c;
          parensStart = cleanHeader.length;
          continue;
        }

        if (stringsEndWith(cleanHeader, c, "=>")) {
          // SINGLEARROW: Found the arrow → The last word in the header (sans "=") is the param.
          const param = RegExpPrototypeExec(/([^\s]+)\s*=$/, StringPrototypeTrim(cleanHeader))?.[1];
          return param ? [param] : [];
        }

        // otherwise keep the character
        cleanHeader += c;
        continue;
      }

      // destructuring
      if (ArrayPrototypeIncludes(complementaryTokensStart, c)) {
        // Inside destructuring argument or initializer expression:
        // Enter node. Push complementary (to be searched for) onto stack.
        const tokenIdx = ArrayPrototypeIndexOf(complementaryTokensStart, c);
        ArrayPrototypePush(ignoreStack, complementaryTokensEnd[tokenIdx]);
        continue; // don't include destructuring in the header
      }
      if (ignoreStack.length) {
        // Inside destructuring or initializer expression.
        if (c === ignoreStack[ignoreStack.length - 1]) {
          // Exit node.
          ArrayPrototypePop(ignoreStack);
        }

        continue; // don't include the destructuring in the header
      }

      if (c === ")") {
        // PARENS: Params end.
        return ArrayPrototypeMap(
          splitBy(StringPrototypeSubstring(cleanHeader, parensStart), ","),
          p => StringPrototypeTrim(p)
        );
      }

      // initializers
      if (c === "=") {
        // Initializer start.
        insideInitializer = true;
        continue; // don't include initializers in the header
      }
      if (insideInitializer) {
        if (c === ",") {
          // Initializer end.
          insideInitializer = false;
          cleanHeader += c; // include the comma in the header.  it's not part of an initializer, it's the separator between params.
        }

        continue;
      }

      // all other characters are kept
      cleanHeader += c;
    }

    // This should not happen, but might.
    log(`[RuntimeError] extractFunctionParameterNames fell through for: ${StringPrototypeSlice(s, 0, 80)}... header=${cleanHeader}`);
    return [];
  } catch (err) {
    log(`[RuntimeError] extractFunctionParameterNames failed for: ${StringPrototypeSlice(s, 0, 80)}...\n ${defaultStack(err) || errorMessage(err)}`);
  }
}

function previewFunction(cdpProperties) {
  const nameProperty = ArrayPrototypeFind(ownProperty(cdpProperties, "result"), prop => ownProperty(prop, "name") == "name");
  const locationProperty = getInternalFunctionLocationProp(cdpProperties);

  if (nameProperty) {
    // RUN-1991: nameProperty.value might not always exist.
    const nameValue = ownProperty(nameProperty, "value");
    this.extra.functionName = (nameValue && ownProperty(nameValue, "value")) || "";
  }

  if (locationProperty) {
    const locationValue = ownProperty(locationProperty, "value");
    const loc = (locationValue && ownProperty(locationValue, "value")) || "";
    if (!loc) {
      warning(`[RUN-1991] previewFunction missing location: ${JSONStringify(nameProperty)}, ${JSONStringify(locationProperty)}`);
    }
    this.extra.functionLocation = createProtocolLocation(loc);
  }

  const description = ownProperty(this.cdpObj, "description");
  if (description) {
    this.extra.functionParameterNames = extractFunctionParameterNames(description);
  }
}



const CustomPreviewers = {
  Array: [previewArray],
  Int8Array: [previewTypedArray],
  Uint8Array: [previewTypedArray],
  Uint8ClampedArray: [previewTypedArray],
  Int16Array: [previewTypedArray],
  Uint16Array: [previewTypedArray],
  Int32Array: [previewTypedArray],
  Uint32Array: [previewTypedArray],
  Float32Array: [previewTypedArray],
  Float64Array: [previewTypedArray],
  BigInt64Array: [previewTypedArray],
  BigUint64Array: [previewTypedArray],
  Map: [previewSetMap],
  WeakMap: [previewSetMap],
  Set: [previewSetMap],
  WeakSet: [previewSetMap],
  RegExp: [previewRegExp],
  Date: [previewDate],
  Error: ErrorProperties,
  EvalError: ErrorProperties,
  RangeError: ErrorProperties,
  ReferenceError: ErrorProperties,
  SyntaxError: ErrorProperties,
  TypeError: ErrorProperties,
  URIError: ErrorProperties,
  Function: [previewFunction],
  AsyncFunction: [previewFunction],
};

/**
 * Get given prop from given object and get its value.
 * Return RRP wrapper.
 * Since we only use this for a small set of well defined
 * props, we emit a warning if the result is undefined or null.
 */
function evalPropRrpNotNull(owner, propKey) {
  try {
    // Running the previewed object's getter is intended here.
    const plainValue = owner[propKey];
    if (plainValue === undefined || plainValue === null) {
      // [RUN-2223] This should not happen.
      const e = new Error("");
      warning(`[RUN-2223] JS evalPropRrpNotNull got ${plainValue} when evaluating ${propKey} on ${typeof owner}, stack=${defaultStack(e)}`);
    }
    return createRrpValueRaw(plainValue);
  } catch (err) {
    warning(`JS evalPropRrpNotNull exception - calling ${typeof propKey === "symbol" ? SymbolPrototypeToString(propKey) : String(propKey)} on ${typeof owner} - ${defaultStack(err) || errorMessage(err)}`);
    return null;
  }
}

function createRrpPropertyDescriptor(cdpProp) {
  // https://chromedevtools.github.io/devtools-protocol/tot/Runtime/#type-PropertyDescriptor
  const { name, value: cdpValue, writable, get, set, configurable, enumerable, symbol } =
    ownProperties(cdpProp, ["name", "value", "writable", "get", "set", "configurable", "enumerable", "symbol"]);

  let rv = buildRrpObjectFromCdpObject(cdpValue);
  rv.name = name;

  let flags = 0;
  if (writable) {
    flags |= 1;
  }
  if (configurable) {
    flags |= 2;
  }
  if (enumerable) {
    flags |= 4;
  }
  if (flags != 7) {
    rv.flags = flags;
  }

  if (get && ownProperty(get, "objectId")) {
    rv.get = registerCdpObject(get);
  }
  if (set && ownProperty(set, "objectId")) {
    rv.set = registerCdpObject(set);
  }

  if (symbol) {
    rv.isSymbol = true;
  }

  return rv;
}

function createProtocolLocation(location) {
  if (!location) {
    return undefined;
  }
  const { scriptId, lineNumber, columnNumber } =
    ownProperties(location, ["scriptId", "lineNumber", "columnNumber"]);
  return [{
    sourceId: scriptId,
    // CDP line numbers are 0-indexed, while RRP line numbers are 1-indexed.
    line: lineNumber + 1,
    column: columnNumber,
  }];
}

function createProtocolFrame(frameId, cdpFrame, topmost) {
  // CDP call frames don't provide detailed type information.
  const { functionName, functionLocation, location, scopeChain, this: cdpThis } =
    ownProperties(cdpFrame, ["functionName", "functionLocation", "location", "scopeChain", "this"]);
  const type = functionName ? "call" : "global";

  let returnValue;
  if (topmost && fromJsHasReturnValue()) {
    returnValue = createRrpValueRaw(fromJsGetReturnValue());
  }

  return {
    frameId,
    type,
    functionName: functionName || undefined,
    functionLocation: createProtocolLocation(functionLocation),
    location: createProtocolLocation(location),
    scopeChain: ArrayPrototypeMap(scopeChain, registerCdpScope),
    this: buildRrpObjectFromCdpObject(cdpThis),
    returnValue,
  };
}

function createRrpScope(scopeId) {
  const cdpScope = getCdpScopeByRrpId(scopeId);

  const { type: scopeType, name: scopeName, object: scopeObject } =
    ownProperties(cdpScope, ["type", "name", "object"]);
  let type;
  switch (scopeType) {
    case "global":
      type = "global";
      break;
    case "with":
      type = "with";
      break;
    default:
      type = scopeName ? "function" : "block";
      break;
  }

  let rrpId, bindings;
  if (type == "global" || type == "with") {
    rrpId = registerCdpObject(scopeObject);
  } else {
    bindings = [];

    const properties = ownProperty(sendCDPMessage("Runtime.getProperties", {
      objectId: ownProperty(scopeObject, "objectId"),
      ownProperties: true,
      generatePreview: false,
      objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP
    }), "result");
    for (let i = 0; i < properties.length; i++) {
      const { name, value: cdpProp } = ownProperties(properties[i], ["name", "value"]);
      const rrpProp = buildRrpObjectFromCdpObject(cdpProp);
      ArrayPrototypePush(bindings, { ...rrpProp, name });
    }
  }

  return {
    scopeId,
    type,
    object: rrpId,
    functionName: scopeName || undefined,
    bindings,
  };
}

/** ###########################################################################
 * {@link DOM_forceLayout}
 * ##########################################################################*/
function DOM_forceLayout() {
  layoutDom();
  return {};
}

/** ###########################################################################
 * {@link DOM_getDocument}
 * ##########################################################################*/
function DOM_getDocument() {
  const rrpId = registerPlainObject(WindowGetDocument(window));

  return {
    data: {},
    document: rrpId
  };
}

/** ###########################################################################
 * {@link DOM_getAllBoundingClientRects}
 * ##########################################################################*/

function getLastBoundingClientRect(nodeRrpId) {
  return MapPrototypeGet(gLastBoundingClientRectsByNodeRrpId, nodeRrpId);
}

/**
 * @see https://static.replay.io/protocol/tot/DOM/#type-NodeBounds
 */
function DOM_getAllBoundingClientRects() {
  const cx = new StackingContext(window);
  cx.addChildren(WindowGetDocument(window));

  const entries = cx.flatten();
  // Get elements in front-to-back order.
  ArrayPrototypeReverse(entries);

  const elements = ArrayPrototypeFilter(
    ArrayPrototypeMap(entries, (elem, i) => {
      const id = registerPlainObject(elem.raw) || i;

      // Use the containing context of the element to find any
      // applicable transform for its offset.
      const transformMatrix =
        elem.containingContext.findAncestor(parent => !!parent.transformMatrix)
          ?.transformMatrix;

      // Offset the bounding client rect by the transform matrix
      // and containing iframe offset (if any).
      let { left, top, right, bottom } = shiftRect(
        plainRect(ElementPrototypeGetBoundingClientRect(elem.raw)),
        elem.offset,
        transformMatrix
      );
      if (left >= right || top >= bottom) {
        return null;
      }

      // Get all client rects.
      const clientRects = [];
      forEachItem(ElementPrototypeGetClientRects(elem.raw), DOMRectListPrototypeGetLength, DOMRectListPrototypeItem, (r) => {
        const { left, top, right, bottom } =
          shiftRect(plainRect(r), elem.offset, transformMatrix);
        ArrayPrototypePush(clientRects, [left, top, right, bottom]);
      });

      const clipBounds =
        shiftRect(elem.clipBounds, elem.offset, transformMatrix);
      // ignore elements that are completely outside their clipBounds
      if (
        clipBounds.left > right ||
        clipBounds.top > bottom ||
        clipBounds.right < left ||
        clipBounds.bottom < top
      ) {
        return null;
      }
      // only return the clipBounds that actually affect this element
      if (clipBounds.left === undefined || clipBounds.left <= left) {
        delete clipBounds.left;
      }
      if (clipBounds.top === undefined || clipBounds.top <= top) {
        delete clipBounds.top;
      }
      if (clipBounds.right === undefined || clipBounds.right >= right) {
        delete clipBounds.right;
      }
      if (clipBounds.bottom === undefined || clipBounds.bottom >= bottom) {
        delete clipBounds.bottom;
      }

      const v = {
        node: id,
        rect: [left, top, right, bottom],
      };
      if (clientRects.length > 0) {
        v.rects = clientRects;
      }
      if (ObjectKeys(clipBounds).length > 0) {
        v.clipBounds = clipBounds;
      }
      if (elem.style && CSSStyleDeclarationPrototypeGetPropertyValue(elem.style, "visibility") === "hidden") {
        v.visibility = "hidden";
      }
      if (elem.style && CSSStyleDeclarationPrototypeGetPropertyValue(elem.style, "pointer-events") === "none") {
        v.pointerEvents = "none";
      }

      MapPrototypeSet(gLastBoundingClientRectsByNodeRrpId, id, v);

      return v;
    }),
    (v) => !!v
  );

  return { elements };
};

/** ###########################################################################
 * {@link DOM_getBoundingClientRect}
 * ##########################################################################*/

/**
 * @see https://static.replay.io/protocol/tot/DOM/#type-BoxModel
 */
function DOM_getBoundingClientRect({ node }) {
  if (!MapPrototypeGetSize(gLastBoundingClientRectsByNodeRrpId)) {
    // compute all basic bounding client rect sizes
    DOM_getAllBoundingClientRects();
  }
  const rects = getNodeBoundingClientRects(node);
  const rect = rects[0];

  return { rect };
}

/** ###########################################################################
 * {@link DOM_getBoxModel}
 * ##########################################################################*/

function getNodeBoundingClientRects(nodeRrpId) {
  const rectInfo = getLastBoundingClientRect(nodeRrpId);
  return rectInfo?.rects ||
    (rectInfo?.rect ?
      [rectInfo.rect] :
      [[0, 0, 20, 20]] // random default rect
    );
}

/**
 * @see https://static.replay.io/protocol/tot/DOM/#type-BoxModel
 */
function DOM_getBoxModel({ node: nodeRrpId }) {
  const nodeObj = getPlainObjectByRrpId(nodeRrpId);

  const model = {
    node: nodeRrpId
  };

  if (fromJsIsBlinkElementObject(nodeObj)) {
    const nodeId = getBlinkNodeIdByRrpId(nodeRrpId);
    /**
     * @see https://chromedevtools.github.io/devtools-protocol/tot/DOM/#type-BoxModel
     */
    const cdpModel = fromJsGetBoxModel(nodeId);

    if (cdpModel) {
      const {
        content, padding, border, margin,
        // width, height, shapeOutside
      } = ownProperties(cdpModel, ["content", "padding", "border", "margin"]);
      ObjectAssign(
        model,
        {
          content,
          padding,
          border,
          margin
        }
      );
    }
  }

  if (!model.content) {
    // The given node does not have a box model.
    // -> Produce a "correct" output to prevent triggering of a session command
    // failure.
    // We do this because this is not technically a failure state. It makes
    // sense for the client to want to get a box model of a node no matter if it
    // has one or not.
    model.content = [];
    model.padding = [];
    model.border = [];
    model.margin = [];
  }

  return { model };
}


/** ###########################################################################
 * {@link DOM_getEventListeners}
 * ##########################################################################*/

function DOM_getEventListeners({ node }) {
  const nodeObject = getPlainObjectByRrpId(node);
  assert(nodeObject);

  const listenerInfos = fromJsCollectEventListeners(nodeObject);

  if (NodePrototypeGetNodeName(nodeObject) == "HTML") {
    // Add event listeners for the document and window as well.
    pushAll(listenerInfos, fromJsCollectEventListeners(NodePrototypeGetParentNode(nodeObject)));   // document
    // pushAll(listenerInfos, fromJsCollectEventListeners(nodeObject.ownerGlobal));  // window
  }

  const listeners = [];
  for (let i = 0; i < listenerInfos.length; i++) {
    const { type, handler, capture } = listenerInfos[i];
    if (!handler) {
      continue;
    }
    ArrayPrototypePush(listeners, {
      node,
      handler: registerPlainObject(handler),
      type,
      capture,
    });
  }

  return { listeners, data: {} };
}

/** ###########################################################################
 * {@link DOM_querySelector}
 * ##########################################################################*/

// querySelector is a separate function on each interface that has it.
function querySelectorOn(node, selector) {
  if (fromJsIsBlinkElementObject(node)) {
    return ElementPrototypeQuerySelector(node, selector);
  }
  try {
    return DocumentPrototypeQuerySelector(node, selector);
  } catch {
    return DocumentFragmentPrototypeQuerySelector(node, selector);
  }
}

function DOM_querySelector({ node, selector }) {
  const nodeObj = getPlainObjectByRrpId(node);

  const resultObj = querySelectorOn(nodeObj, selector);
  if (!resultObj) {
    return { data: {} };
  }
  const result = registerPlainObject(resultObj);
  return { result, data: {} };
}

/** ###########################################################################
 * {@link DOM_performSearch}
 * ##########################################################################*/

function DOM_performSearch({ query }) {
  query = StringPrototypeTrim(query);
  const nodeObjects = fromJsDomPerformSearch(query);
  const nodeRrpIds = nodeObjects
    ? ArrayPrototypeMap(nodeObjects, registerPlainObject)
    : [];

  return { nodes: nodeRrpIds, data: {} };
}


/** ###########################################################################
 * {@link CSS_getComputedStyle}
 * ##########################################################################*/

function CSS_getComputedStyle({ node }) {
  const nodeObj = getPlainObjectByRrpId(node);

  const computedStyle = [];
  if (fromJsIsBlinkElementObject(nodeObj)) {
    // NOTE: tested successfully for same-CSP elements of different iframes
    const ownerGlobal = window;

    // TODO: add pseudoType support - https://linear.app/replay/issue/RUN-953

    let styleInfo;
    // const pseudoType = getPseudoType(node);
    // if (pseudoType) {
    //   styleInfo = ownerGlobal.getComputedStyle(
    //     nodeObj.parentNode,
    //     pseudoType
    //   );
    // }
    // else {
    styleInfo = WindowGetComputedStyle(ownerGlobal, nodeObj);
    const styleInfoLength = CSSStyleDeclarationPrototypeGetLength(styleInfo);
    for (let i = 0; i < styleInfoLength; i++) {
      ArrayPrototypePush(computedStyle, {
        name: CSSStyleDeclarationPrototypeItem(styleInfo, i),
        value: CSSStyleDeclarationPrototypeGetPropertyValue(styleInfo, CSSStyleDeclarationPrototypeItem(styleInfo, i)),
      });
    }
  }
  return { computedStyle };
}



/** ###########################################################################
 * {@link CSS_getAppliedRules}
 * ##########################################################################*/

/**
 *
 * @see https://developer.mozilla.org/en-US/docs/Web/API/CSSRule
 * @see https://developer.mozilla.org/en-US/docs/Web/API/CSSStyleRule
 * @see https://chromedevtools.github.io/devtools-protocol/tot/CSS/#type-CSSRule
 * @see https://static.replay.io/protocol/tot/CSS/#type-Rule
 */
function registerCdpAsRrpCssRule(nodeObj, cdpRule) {
  // NOTE: type is deprecated -> don't care
  const type = 1;
  const {
    selectorList,
    styleSheetId: styleSheetCpdId,
    style: cdpStyle,
    range: ruleRange,
    origin
  } = ownProperties(cdpRule || {}, ["selectorList", "styleSheetId", "style", "range", "origin"]);
  let {
    cssText: styleCssText,
    range: styleRange,
    cssProperties
  } = ownProperties(cdpStyle || {}, ["cssText", "range", "cssProperties"]);


  let styleSheetRrpId;
  if (styleSheetCpdId) {
    styleSheetRrpId = MapPrototypeGet(gRrpIdByCdpId, styleSheetCpdId);
    if (!styleSheetRrpId) {
      const nativeSheet = fromJsCssGetStylesheetByCpdId(styleSheetCpdId);

      // NOTE: `isSystem` is part of RRP from `gecko`.
      //    -> Chromium has a more diversified `StyleSheetOrigin` enum for this,
      //      (that is only accessible on the rule level in CDP, for some reason)
      const isSystem = origin !== 'regular';
      const styleSheet = { isSystem };
      if (nativeSheet?.href) {
        styleSheet.href = nativeSheet.href;
      }

      const styleSheetPreview = {
        className: 'RRPStyleSheetPreview', // no pre-defined className
        preview: {
          overflow: true,
          styleSheet
        }
      };
      styleSheetRrpId = registerRrpPreview(styleSheetPreview, nativeSheet);
      registerRrpCpdId(styleSheetRrpId, styleSheetCpdId);
    }
  }


  // stylePreview

  const properties = ArrayPrototypeMap(
    // ignore props without text presentation
    ArrayPrototypeFilter(cssProperties || [], prop => !!ownProperty(prop, "text")),
    prop => {
      const { name, value, important } = ownProperties(prop, ["name", "value", "important"]);
      return {
        name,
        value,
        important
      };
    }
  );
  /**
   * hackfix: for some reason, `user-agent` (and possibly other) styles don't have `cssText`.
   *    So, for now, we cook up a simple css serialization algo here.
   *    Native chromium has a better solution of course.
   * @see https://source.chromium.org/chromium/chromium/src/+/main:third_party/blink/renderer/core/css/style_property_serializer.cc;l=251;drc=3decef66bc4c08b142a19db9628e9efe68973e64
   * @see https://source.chromium.org/chromium/chromium/src/+/main:third_party/blink/renderer/core/css/style_property_serializer.cc;l=204;drc=3decef66bc4c08b142a19db9628e9efe68973e64
   */
  if (!styleCssText) {
    styleCssText = '\n  ' + ArrayPrototypeJoin(
      ArrayPrototypeMap(properties, ({ name, value, important }) => {
        const suffix = important ? ' !important' : '';
        return `${name}: ${value}${suffix};`;
      }),
      '\n  '
    );
  }
  const stylePreview = {
    className: 'CSS2Properties', // `gecko` naming convention
    preview: {
      overflow: true,
      style: {
        cssText: styleCssText,
        parentRule: 0, // filled in once we have it, below
        properties
      }
    }
  };
  const nativeStlyeDeclaration = null;
  const styleRrpId = registerRrpPreview(stylePreview, nativeStlyeDeclaration);


  // rulePreview

  const range = ruleRange || styleRange;
  const maybeStartLine = range ? ownProperty(range, "startLine") : undefined;

  // Lines from CDB data are zero-based.
  const startLine = maybeStartLine != null ? maybeStartLine + 1 : maybeStartLine;
  const startColumn = range ? ownProperty(range, "startColumn") : undefined;
  // see https://static.replay.io/protocol/tot/CSS/#type-OriginalStyleSheetLocation
  const originalLocation = undefined; // TODO
  const selectorText = (selectorList && ownProperty(selectorList, "text")) || '';

  /**
   * Based on `CSSStyleRule::cssText()`.
   * @see https://github.com/replayio/chromium/blob/052831f0220b79fe0c3343b49f6d2863ea6de05d/third_party/blink/renderer/core/css/css_style_rule.cc#L94
   */
  const ruleCssText = `${selectorText} {${styleCssText}}`;

  const rulePreview = {
    className: 'CSSRule',
    preview: {
      overflow: true,
      rule: {
        type,
        cssText: ruleCssText,
        parentStyleSheet: styleSheetRrpId,
        startLine,
        startColumn,
        originalLocation,
        selectorText,
        style: styleRrpId
      }
    }
  };

  // NOTE: we cannot currently lookup the native `CSSRule` object because
  //      InspectorCSSAgent::BuildObjectForRuleWithoutMedia does not
  //      store an id.
  // const nativeRule = lookupNativeCssRuleByCdpRule();
  const nativeRule = null;
  const ruleRrpId = registerRrpPreview(rulePreview, nativeRule);

  // set ruleRrpId
  stylePreview && (stylePreview.preview.style.parentRule = ruleRrpId);

  return ruleRrpId;
}


/**
 * NOTE1: RRP's `CSS.Rule` is based on how gecko does things.
 *    gecko has a utility function to produce the rules in one call.
 *    But in chromium, we have to query and convert the data in multiple steps.
 *
 *
 * @see https://chromedevtools.github.io/devtools-protocol/tot/CSS/#method-getMatchedStylesForNode
 *
 * @see https://linear.app/replay/issue/RUN-981/enhance-pausegetobjectpreview-css-previews
 * @see https://github.com/replayio/gecko-dev/blob/628cc55f22785f3a66a8c767cdc86f31feb9a050/layout/inspector/InspectorUtils.cpp#L155
 */
function convertCdpToRrpCssRules(nodeObj, cdpMatchedStyles) {
  const appliedRules = [];

  const {
    matchedRules = EmptyArray,
    pseudoIdMatches = EmptyArray
  } = ownProperties(cdpMatchedStyles, ["matchedRules", "pseudoIdMatches"]);

  function addCdpRule(cdpRule, pseudoElement = undefined) {
    const rrpRuleId = registerCdpAsRrpCssRule(nodeObj, cdpRule);
    const appliedRule = {
      rule: rrpRuleId,
      pseudoElement
    };
    ArrayPrototypePush(appliedRules, appliedRule);
  }

  ArrayPrototypeReverse(matchedRules);
  for (let i = 0; i < matchedRules.length; i++) {
    addCdpRule(ownProperty(matchedRules[i], "rule"));
  }

  for (let i = 0; i < pseudoIdMatches.length; i++) {
    const pseudoMatch = pseudoIdMatches[i];
    const {
      // see: https://chromedevtools.github.io/devtools-protocol/tot/DOM/#type-PseudoType
      pseudoType,
      // pseudoIdentifier,
      matches
    } = ownProperties(pseudoMatch, ["pseudoType", "matches"]);
    ArrayPrototypeReverse(matches);
    for (let j = 0; j < matches.length; j++) {
      addCdpRule(ownProperty(matches[j], "rule"), pseudoType);
    }
  }

  return appliedRules;
}

function CSS_getAppliedRules({ node: nodeRrpId }) {
  const nodeObj = getPlainObjectByRrpId(nodeRrpId);

  let rules = MapPrototypeGet(gCssRulesByNodeRrpId, nodeRrpId);
  const data = {};

  if (!rules && fromJsIsBlinkNodeObject(nodeObj)) {
    const nodeId = getBlinkNodeIdByRrpId(nodeRrpId);

    // NOTE: CDP CSS domain commands are not enabled, so we have to get the data indirectly.
    // const cdpMatchedStyles = sendCDPMessage('CSS.getMatchedStylesForNode', { nodeId });
    if (fromJsIsBlinkElementObject(nodeObj)) {
      const cdpMatchedStyles = fromJsGetMatchedStylesForElement(nodeId) || { };
      rules = convertCdpToRrpCssRules(nodeObj, cdpMatchedStyles);
    } else {
      rules = [];
    }
    MapPrototypeSet(gCssRulesByNodeRrpId, nodeRrpId, rules);
  } else {
    // The target is not a node.
    log(`[RuntimeWarning] CSS.getAppliedRules called with non-node: ${nodeRrpId} ${isBlinkObject(nodeObj)} ${typeof nodeObj}.`);
    rules = [];
  }

  return { rules, data };
}

/** ###########################################################################
 * StackingContext
 * ##########################################################################*/
// Mouse Targets Overview
//
// Mouse target data is used to figure out which element to highlight when the
// mouse is hovered/clicked on different parts of the screen when the element
// picker is used. To determine this, we need to know the bounding client rects
// of every element (easy) and the order in which different elements are stacked
// (not easy).
//
// To figure out the order in which elements are stacked, we reconstruct the
// stacking contexts on the page and the order in which elements are laid out
// within those stacking contexts, allowing us to assemble a sorted array of
// elements such that for any two elements that overlap, the frontmost element
// appears first in the array.
//
// References:
//
// https://www.w3.org/TR/CSS21/zindex.html
//
//   We try to follow this reference, although not all of its rules are
//   implemented yet.
//
// https://developer.mozilla.org/en-US/docs/Web/CSS/CSS_Positioning/Understanding_z_index/The_stacking_context
//
//   This is helpful but the rules for when stacking contexts are created are
//   quite baroque and don't seem to match up with the spec above, so they are
//   mostly ignored here.

// Information about an element needed to add it to a stacking context.
function StackingContextElement(
  containingContext,
  node,
  parent,
  offset,
  style,
  clipBounds
) {
  // The stacking context this element is contained within.
  this.containingContext = containingContext;

  // Underlying element.
  this.raw = node;

  // Offset relative to the outer window of the window containing this context.
  this.offset = offset;

  // the parent StackingContextElement
  this.parent = parent;

  // Style and clipping information for the node.
  this.style = style;
  this.clipBounds = clipBounds;

  // Any stacking context at which this element is the root.
  this.context = null;
}

StackingContextElement.prototype = {
  isPositioned() {
    return CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "position") != "static";
  },

  isAbsolutelyPositioned() {
    return ArrayPrototypeIncludes(["absolute", "fixed"], CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "position"));
  },

  isTable() {
    return ArrayPrototypeIncludes(["table", "inline-table"], CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "display"));
  },

  isFlexOrGridContainer() {
    return ArrayPrototypeIncludes(["flex", "inline-flex", "grid", "inline-grid"],
      CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "display")
    );
  },

  isBlockElement() {
    return ArrayPrototypeIncludes(["block", "table", "flex", "grid"], CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "display"));
  },

  isFloat() {
    return CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "float") != "none";
  },

  getPositionedAncestor() {
    if (this.isPositioned()) {
      return this;
    }
    return this.parent?.getPositionedAncestor();
  },

  // see https://developer.mozilla.org/en-US/docs/Web/Guide/CSS/Block_formatting_context
  getFormattingContextElement() {
    if (!this.parent) {
      return this;
    }
    if (this.isFloat()) {
      return this;
    }
    if (this.isAbsolutelyPositioned()) {
      return this;
    }
    if (
      ArrayPrototypeIncludes([
        "inline-block",
        "table-cell",
        "table-caption",
        "table",
        "table-row",
        "table-row-group",
        "table-header-group",
        "table-footer-group",
        "inline-table",
        "flow-root",
      ], CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "display"))
    ) {
      return this;
    }
    if (
      this.isBlockElement() &&
      !(
        ArrayPrototypeIncludes(["visible", "clip"], CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "overflow-x")) &&
        ArrayPrototypeIncludes(["visible", "clip"], CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "overflow-y"))
      )
    ) {
      return this;
    }
    if (ArrayPrototypeIncludes(["layout", "content", "paint"], CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "contain"))) {
      return this;
    }
    if (this.parent.isFlexOrGridContainer() && !this.isFlexOrGridContainer() && !this.isTable()) {
      return this;
    }
    if (
      CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "column-count") != "auto" ||
      CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "column-width") != "auto"
    ) {
      return this;
    }
    if (CSSStyleDeclarationPrototypeGetPropertyValue(this.style, "column-span") == "all") {
      return this;
    }
    return this.parent.getFormattingContextElement();
  },

  // toString() {
  //   return getObjectIdRaw(this.raw);
  // },
};

let gNextStackingContextId = 1;

// Information about all the nodes in the same stacking context.
// The spec says that some elements should be treated as if they
// "created a new stacking context, but any positioned descendants and
// descendants which actually create a new stacking context should be
// considered part of the parent stacking context, not this new one".
// For these elements we also create a StackingContext but pass the
// parent stacking context to the constructor as the "realStackingContext".
function StackingContext(window, options) {
  const {
    parentContext,
    root,
    offset,
    transformMatrix,
    realStackingContext
  } = options || {};
  this.window = window;
  this.parentContext = parentContext;
  this.id = gNextStackingContextId++;

  this.realStackingContext = realStackingContext || this;

  // Offset relative to the outer window of the window containing this context.
  this.offset = offset || { left: 0, top: 0 };

  // Transform scale parameter.  This is only relevant for stacking
  // contexts for IFRAME elements.
  if (transformMatrix) {
    assert(root && ElementPrototypeGetTagName(root.raw) === "IFRAME");
  }
  this.transformMatrix = transformMatrix;

  // The arrays below are filled in tree order (preorder depth first traversal).

  // All non-positioned, non-floating elements.
  this.nonPositionedElements = [];

  // All floating elements.
  this.floatingElements = [];

  // All positioned elements with an auto or zero z-index.
  this.positionedElements = [];

  // Arrays of elements with non-zero z-indexes, indexed by that z-index.
  this.zIndexElements = new Map();

  this.root = root;
  if (root) {
    this.addChildrenWithParent(root);
  }
}

let gStackingContextWarn = 0;

StackingContext.prototype = {
  toString() {
    return `StackingContext:${this.id}`;
  },

  // Find the first parent stacking context matching the predicate.
  findAncestor(predicate) {
    let cur = this;
    while (cur) {
      if (predicate(cur)) {
        return cur;
      }
      cur = cur.parentContext;
    }
    return null;
  },

  // Add node and its descendants to this stacking context.
  add(node, parentElem, offset) {
    const style = WindowGetComputedStyle(this.window, node);
    if (!style) {
      // It's not 100% clear why this is sometimes null, but it seems like
      // this can happen if DOM commands are sent when the window is shutting
      // down in some way or another.
      return;
    }

    const position = CSSStyleDeclarationPrototypeGetPropertyValue(style, "position");
    let clipBounds;
    if (position == "absolute") {
      clipBounds = parentElem?.getPositionedAncestor()?.clipBounds || {};
    } else if (position == "fixed") {
      clipBounds = {};
    } else {
      clipBounds = parentElem?.clipBounds || {};
    }
    clipBounds = ObjectAssign({}, clipBounds);
    const cx = new StackingContextElement(this, node, parentElem, offset, style, clipBounds);
    if (!ArrayPrototypeIncludes(["HTML", "BODY"], ElementPrototypeGetTagName(cx.raw))) {
      if (CSSStyleDeclarationPrototypeGetPropertyValue(style, "overflow-x") != "visible") {
        const clipBounds2 = plainRect(ElementPrototypeGetBoundingClientRect(cx.getFormattingContextElement().raw));
        cx.clipBounds.left =
          clipBounds.left !== undefined
            ? MathMax(clipBounds2.left, clipBounds.left)
            : clipBounds2.left;
        cx.clipBounds.right =
          clipBounds.right !== undefined
            ? MathMin(clipBounds2.right, clipBounds.right)
            : clipBounds2.right;
      }
      if (CSSStyleDeclarationPrototypeGetPropertyValue(style, "overflow-y") != "visible") {
        const clipBounds2 = plainRect(ElementPrototypeGetBoundingClientRect(cx.getFormattingContextElement().raw));
        cx.clipBounds.top =
          clipBounds.top !== undefined
            ? MathMax(clipBounds2.top, clipBounds.top)
            : clipBounds2.top;
        cx.clipBounds.bottom =
          clipBounds.bottom !== undefined
            ? MathMin(clipBounds2.bottom, clipBounds.bottom)
            : clipBounds2.bottom;
      }
    }

    // Create a new stacking context for any iframes.
    const contentWindow = ElementPrototypeGetTagName(cx.raw) == "IFRAME" ? HTMLIFrameElementPrototypeGetContentWindow(cx.raw) : undefined;
    const contentDocument = contentWindow ? WindowGetDocument(contentWindow) : undefined;
    if (contentDocument) {
      let { left, top } = plainRect(ElementPrototypeGetBoundingClientRect(cx.raw));

      // The left and top are adjusted by the transform matrix for
      // the containing iframe, if any.  For this, we just search up the
      // context chain, looking for one with a defined transform matrix.
      let parentTransformMatrix =
        this.findAncestor(parent => !!parent.transformMatrix)
          ?.transformMatrix;
      if (parentTransformMatrix) {
        const adjusted = adjustCoordinateByTransformMatrix(
          [ left, top ],
          parentTransformMatrix
        );
        left = adjusted[0];
        top = adjusted[1];
      }

      // Compute the transform matrix for the iframe within its containing
      // document.
      // If we have a parent transform matrix, multiply it with this one.
      let transformMatrix = computeTransformMatrix(cx.raw, this.window);
      if (parentTransformMatrix) {
        transformMatrix = multiplyTransformMatrix(
          parentTransformMatrix,
          transformMatrix
        );
      }

      this.addContext(cx, undefined, { left, top, transformMatrix });
      cx.context.addChildren(contentDocument);
    }

    if (!cx.style) {
      this.addNonPositionedElement(cx);
      this.addChildrenWithParent(cx);
      return;
    }

    const parentDisplay = cx.parent?.style ? CSSStyleDeclarationPrototypeGetPropertyValue(cx.parent.style, "display") : undefined;
    if (
      position != "static" ||
      ArrayPrototypeIncludes(["flex", "inline-flex", "grid", "inline-grid"], parentDisplay)
    ) {
      const zIndex = CSSStyleDeclarationPrototypeGetPropertyValue(cx.style, "z-index");
      if (zIndex != "auto") {
        this.addContext(cx, undefined, {});
        // Elements with a zero z-index have their own stacking context but are
        // grouped with other positioned children with an auto z-index.
        const index = +zIndex | 0;
        if (index) {
          this.realStackingContext.addZIndexElement(cx, index);
          return;
        }
      }

      if (position != "static") {
        this.realStackingContext.addPositionedElement(cx);
        if (!cx.context) {
          this.addContext(cx, this.realStackingContext, {});
        }
      } else {
        this.addNonPositionedElement(cx);
        if (!cx.context) {
          this.addChildrenWithParent(cx);
        }
      }
      return;
    }

    if (cx.isFloat()) {
      // Group the element and its descendants.
      this.addContext(cx, this.realStackingContext, {});
      this.addFloatingElement(cx);
      return;
    }

    const display = CSSStyleDeclarationPrototypeGetPropertyValue(cx.style, "display");
    if (display == "inline-block" || display == "inline-table") {
      // Group the element and its descendants.
      this.addContext(cx, this.realStackingContext, {});
      this.addNonPositionedElement(cx);
      return;
    }

    // Handle opacity-based stacking context creation _after_
    // we check for positioned elements.
    // This is on the assumption that the rules for floating and
    // positioned elements should apply before the rules for opacity.

    // Elements with `opacity < 1` get their own stacking context.
    let opacity = 1;
    const opacityStr = CSSStyleDeclarationPrototypeGetPropertyValue(cx.style, "opacity");
    if (opacityStr !== undefined && opacityStr !== "") {
      opacity = +opacityStr;
    }
    if (opacity < 1) {
      this.addContext(cx, undefined, {});
    }

    this.addNonPositionedElement(cx);
    this.addChildrenWithParent(cx);
  },

  addContext(
    elem,
    realStackingContext,
    { left, top, transformMatrix } = {}
  ) {
    if (elem.context) {
      assert(!left && !top, "!left && !top");
      return;
    }

    left = left || 0;
    top = top || 0;

    const offset = {
      left: this.offset.left + left,
      top: this.offset.top + top,
    };
    elem.context = new StackingContext(this.window, {
      parentContext: this,
      root: elem,
      offset,
      realStackingContext,
      transformMatrix
    });
  },

  addZIndexElement(elem, index) {
    const existing = MapPrototypeGet(this.zIndexElements, index);
    if (existing) {
      ArrayPrototypePush(existing, elem);
    } else {
      MapPrototypeSet(this.zIndexElements, index, [elem]);
    }
  },

  addPositionedElement(elem) {
    ArrayPrototypePush(this.positionedElements, elem);
  },

  addFloatingElement(elem) {
    ArrayPrototypePush(this.floatingElements, elem);
  },

  addNonPositionedElement(elem) {
    ArrayPrototypePush(this.nonPositionedElements, elem);
  },

  addChildren(parentNode) {
    const children = childrenOf(parentNode);
    if (!children) {
      return;
    }
    forEachItem(children, HTMLCollectionPrototypeGetLength, HTMLCollectionPrototypeItem, (child) => {
      if (!fromJsIsBlinkElementObject(child)) {
        return;
      }
      this.add(child, undefined, this.offset);
    });
  },

  addChildrenWithParent(cx) {
    const children = childrenOf(cx.raw);
    if (!children) {
      // [TT-253] `cx.raw` should always be an Element and
      // Element.prototype.children should always return an `HTMLCollection`.
      // Not sure why it sometimes complains about not being iterable.
      if (!fromJsIsBlinkElementObject(cx.raw) && !gStackingContextWarn) {
        ++gStackingContextWarn;
        warning(
          `[TT-253] cx.raw should be Element but is not: ${describeValueShape(cx.raw)}, children=${describeValueShape(children)}`
        );
      }
      return;
    }
    forEachItem(children, HTMLCollectionPrototypeGetLength, HTMLCollectionPrototypeItem, (child) => {
      if (!fromJsIsBlinkElementObject(child)) {
        return;
      }
      this.add(child, cx, this.offset);
    });
  },

  // Get the elements in this context ordered back-to-front.
  flatten() {
    /**
     * @type {StackingContextElement[]}
     */
    const rv = [];

    const pushElements = (elems) => {
      for (let i = 0; i < elems.length; i++) {
        const elem = elems[i];
        if (elem.context && elem.context != this) {
          pushAll(rv, elem.context.flatten());
        } else {
          ArrayPrototypePush(rv, elem);
        }
      }
    };

    const pushZIndexElements = (filter) => {
      for (let i = 0; i < zIndexes.length; i++) {
        const z = zIndexes[i];
        if (filter(z)) {
          pushElements(MapPrototypeGet(this.zIndexElements, z));
        }
      }
    };

    const zIndexes = mapKeysArray(this.zIndexElements);
    ArrayPrototypeSort(zIndexes, (a, b) => a - b);

    if (this.root) {
      pushElements([this.root]);
    }
    pushZIndexElements((z) => z < 0);
    pushElements(this.nonPositionedElements);
    pushElements(this.floatingElements);
    pushElements(this.positionedElements);
    pushZIndexElements((z) => z > 0);

    return rv;
  },
};

/** ###########################################################################
 * {@link shiftRect}
 * ##########################################################################*/
function shiftRect(rect, offset, transformMatrix) {
  // Apply the transform to the rect, before offsetting it.
  // The offset has already been adjusted as needed by any transforms
  // that apply to it.
  let { left, top, right, bottom } = rect;
  if (transformMatrix) {
    if (left && top) {
      const leftTop = adjustCoordinateByTransformMatrix(
        [ left, top ],
        transformMatrix
      );
      const leftTrans = leftTop[0], topTrans = leftTop[1];
      left = leftTrans;
      top = topTrans;
    }
    if (right && bottom) {
      const rightBottom = adjustCoordinateByTransformMatrix(
        [ right, bottom ],
        transformMatrix
      );
      const rightTrans = rightBottom[0], bottomTrans = rightBottom[1];
      right = rightTrans;
      bottom = bottomTrans;
    }
  }

  left = rect.left !== undefined ?  offset.left + left : undefined;
  top = rect.top !== undefined ?  offset.top + top : undefined;
  right = rect.right !== undefined ?  offset.left + right : undefined;
  bottom = rect.bottom !== undefined ?  offset.top + bottom : undefined;
  return { left, top, right, bottom };
}

/** ###########################################################################
 * {@link parseCssTransformStringToMatrix}
 * Parses a CSS transform string into an 6-element array representing a 2D
 * transformation matrix.
 * ```
 * [ scaleX, skewX, skewY, scaleY, translateX, translateY ]
 * ```
 * On error, returns undefined.
 * ##########################################################################*/
function parseCssTransformStringToMatrix(transform) {
  if (!transform || transform === "none") {
    return;
  }
  try {
    // see https://developer.mozilla.org/en-US/docs/Web/API/CSSStyleValue/parse_static
    const parsedTransform = CSSStyleValueParse(
      "transform",
      transform,
    );
    // FIXME: We only handle 2D transforms for now.
    if (!CSSTransformValuePrototypeGetIs2D(parsedTransform)) {
      return;
    }
    if (CSSTransformValuePrototypeGetLength(parsedTransform) > 0) {
      const { a, b, c, d, e, f } = CSSTransformComponentPrototypeToMatrix(parsedTransform[0]);
      return [a, b, c, d, e, f];
    }
  } catch (err) {
    // FIXME: log a command diagnostic / warning.
  }
}

/** ###########################################################################
 * {@link computeTransformMatrix}
 * Compute the full transform for an element within its containing document.
 * ##########################################################################*/
function computeTransformMatrix(element, window) {
  let curMatrix = [1,0,0,1,0,0]; // start with identity matrix
  let curElem = element;
  while(curElem && fromJsIsBlinkElementObject(curElem)) {
    const transformStr = CSSStyleDeclarationPrototypeGetPropertyValue(WindowGetComputedStyle(window, curElem), "transform");
    const transformMatrix = parseCssTransformStringToMatrix(transformStr);
    if (transformMatrix) {
      curMatrix = multiplyTransformMatrix(transformMatrix, curMatrix);
    }
    curElem = NodePrototypeGetParentNode(curElem);
  }
  return curMatrix;
}

/** ###########################################################################
 * {@link multiplyTransformMatrix}
 * Multiply two transform matrices of teh form [a, b, c, d, tx, ty]
 * ##########################################################################*/
function multiplyTransformMatrix(m1,m2) {
  // [a, b, c, d, tx, ty] => [
  //   a, c, tx,
  //   b, d, ty,
  //   0, 0, 1
  // ]
  const a1 = m1[0], b1 = m1[1], c1 = m1[2], d1 = m1[3], tx1 = m1[4], ty1 = m1[5];
  const a2 = m2[0], b2 = m2[1], c2 = m2[2], d2 = m2[3], tx2 = m2[4], ty2 = m2[5];

  const a3 = a1 * a2 + c1 * b2;
  const b3 = b1 * a2 + d1 * b2;
  const c3 = a1 * c2 + c1 * d2;
  const d3 = b1 * c2 + d1 * d2;
  const tx3 = a1 * tx2 + c1 * ty2 + tx1;
  const ty3 = b1 * tx2 + d1 * ty2 + ty1;
  return [a3, b3, c3, d3, tx3, ty3];
}

/** ###########################################################################
 * {@link adjustCoordinateByTransformMatrix}
 * Adjust a { left, top } coordinate by a transform matrix
 * ##########################################################################*/
function adjustCoordinateByTransformMatrix(coord, m) {
  const x = coord[0], y = coord[1];
  const scaleX = m[0], skewX = m[1], skewY = m[2], scaleY = m[3], translateX = m[4], translateY = m[5];

  const x2 = x * scaleX + y * skewX + translateX;
  const y2 = x * skewY + y * scaleY + translateY;
  return [x2, y2];
}

/** ###########################################################################
 * Export internal methods via `__RECORD_REPLAY_ARGUMENTS__`.
 * This is to be used for internal debugging purposes.
 * ##########################################################################*/

// TODO: Get rid of `internal`. There is no reason to have this in addition to
//       __RECORD_REPLAY_ARGUMENTS__ and __RECORD_REPLAY__.
__RECORD_REPLAY_ARGUMENTS__.internal = {
  getBlinkNodeIdByRrpId,
  getCdpObjectByRrpId,
  fromJsGetNodeIdByCpdId,
  getPlainObjectByRrpId,
  registerPlainObject,
  gLastBoundingClientRectsByNodeRrpId,
  sendCDPMessage,
  getNextStackingContextId: () => gNextStackingContextId,
  setNextStackingContextId: (id) => { gNextStackingContextId = id; },
  updateNextStackingContextId: (f) => { gNextStackingContextId = f(gNextStackingContextId); },
};

/** ###########################################################################
 * {@link replayEval}
 * ##########################################################################*/

/**
 * Execute a function but only when replaying and with events disallowed.
 */
function replayEval(fn) {
  const {
    beginReplayCode,
    endReplayCode
  } = __RECORD_REPLAY_ARGUMENTS__;
  beginReplayCode("replayEval");
  try {
    // We cannot currently avoid a user-supplied function from getting
    // instrumented. Stringifying and evaling it with events disallowed
    // fixes that problem.
    let fnExpr = StringPrototypeTrim(FunctionPrototypeToString(fn));
    eval(`(${fnExpr})()`);
  } catch (err) {
    // Note: We MUST NOT let this error escape, or it will cause a mismatch in
    // `Runtime_UnwindAndFindExceptionHandler`.
    // TODO: We should just crash here, since its the responsibility of the
    // caller of replayEval to make sure the cb won't throw.
    warning(`replayEval ERROR: ${defaultStack(err) || errorMessage(err)}`);
  } finally {
    endReplayCode();
  }
}

/** ###########################################################################
 * Export JS API methods via `__RECORD_REPLAY__`.
 * This is officially available for scripts in `eval*` commands to use.
 * ##########################################################################*/
Object.assign(__RECORD_REPLAY__, {
  getProtocolIdForObject(obj) {
    return registerPlainObject(obj);
  },
  getObjectFromProtocolId(rrpId) {
    return getPlainObjectByRrpId(rrpId);
  },
  executeCommand,
  log,
  warning,
  getFrameArgumentsArray,
  getCurrentEvaluateFrame,
  replayEval,
  forceCheckpoint,
  forTestingSerializeValueToArray
});

/** ###########################################################################
 * {@link patchReplayApi} decorates our API objects/functions with extra
 * diagnostics, e.g. whether the Replay API was called at all.
 * ##########################################################################*/

let usedReplayApi = 0;

function onReplayApiUsed() {
  ++usedReplayApi;
}

function onReplayApiReset() {
  usedReplayApi = 0;
}

function patchReplayApi() {
  patchReplayApiObject(__RECORD_REPLAY__);
  patchReplayApiObject(__RECORD_REPLAY_ARGUMENTS__);
  patchReplayApiObject(__RECORD_REPLAY_ARGUMENTS__.internal);
}

function patchReplayApiObject(obj) {
  for (const key in obj) {
    const value = obj[key];
    if (isFunction(value)) {
      obj[key] = wrapReplayApiFunction(value);
    }
  }
}

function wrapReplayApiFunction(fn) {
  return (...args) => {
    onReplayApiUsed();
    return ReflectApply(fn, undefined, args);
  };
}


///////////////////////////////////////////////////////////////////////////////
// main.js
///////////////////////////////////////////////////////////////////////////////

patchReplayApi();
initMessages();
addEventListener("Runtime.consoleAPICalled", onConsoleAPICall);
addEventListener("Runtime.executionContextCreated", (params) => {
  const context = ownProperty(params, "context");
  MapPrototypeSet(gExecutionContexts, ownProperty(context, "id"), context);
  SetPrototypeForEach(gContextChangeCallbacks, callback => callback(context, "add"));
});
addEventListener("Runtime.executionContextDestroyed", (params) => {
  const executionContextId = ownProperty(params, "executionContextId");
  const context = MapPrototypeGet(gExecutionContexts, executionContextId);
  SetPrototypeForEach(gContextChangeCallbacks, callback => callback(context, "remove"));
  MapPrototypeDelete(gExecutionContexts, executionContextId);
});
addEventListener("Runtime.executionContextsCleared", () => {
  MapPrototypeForEach(gExecutionContexts, context => {
    SetPrototypeForEach(gContextChangeCallbacks, callback => callback(context, "remove"));
  });
  MapPrototypeClear(gExecutionContexts);
});
sendCDPMessage("Runtime.enable");

})();
