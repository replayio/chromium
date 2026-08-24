// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/bindings/record_replay_throw.h"

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#include "third_party/blink/renderer/platform/bindings/v8_throw_exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"

namespace blink {

bool RecordReplayThrowIfEventsUnavailable(ExceptionState& exception_state,
                                          const char* message) {
  if (!recordreplay::AreEventsUnavailable())
    return false;
  exception_state.ThrowTypeError(message);
  return true;
}

bool RecordReplayThrowIfEventsUnavailable(const char* message) {
  if (!recordreplay::AreEventsUnavailable())
    return false;
  // Best-effort: only throw when script is on the stack. Without an entered
  // context the exception would surface at an unrelated later V8 entry.
  v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
  if (isolate && isolate->InContext() &&
      !ScriptForbiddenScope::IsScriptForbidden()) {
    v8::HandleScope handle_scope(isolate);
    V8ThrowException::ThrowTypeError(isolate, message);
  }
  return true;
}

}  // namespace blink
