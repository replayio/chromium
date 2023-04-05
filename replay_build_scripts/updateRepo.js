const { syncRepo } = require("./syncRepo");

export function updateRepo() {
  const chromium = process.cwd();

  const branch = process.env["BUILDKITE_BRANCH"];

  syncRepo(chromium, `origin/${branch}`);

  const deps = getChromiumDeps();

  syncRepo(path.join(chromium, "v8"), deps.v8);

  syncRepo(path.join(chromium, "third_party", "skia"), deps.skia);

  syncRepo(path.join(chromium, "third_party", "webrtc"), deps.webrtc);

  syncRepo(
    path.join(chromium, "third_party", "boringssl", "src"),
    deps.boringssl
  );
}

function getChromiumDeps() {
  const text = fs.readFileSync("DEPS", "utf8");
  let results = {
    v8: "",
    skia: "",
    webrtc: "",
    boringssl: "",
  };

  let match = /'v8_revision': '(.*?)'/.exec(text);
  assert(match, "Could not find V8 revision");
  results.v8 = match[1];

  match = /'skia_revision': '(.*?)'/.exec(text);
  assert(match, "Could not find skia revision");
  results.skia = match[1];

  match =
    /'https:\/\/github.com\/replayio\/chromium-webrtc.git' \+ '@' \+ '(.*?)'/.exec(
      text
    );
  assert(match, "Could not find webrtc revision");
  results.webrtc = match[1];

  match = /'boringssl_revision': '(.*?)'/.exec(text);
  assert(match, "Could not find boringssl revision");
  results.boringssl = match[1];

  return results;
}

function assert(value, msg) {
  if (!value) {
    throw new Error(msg);
  }
}
