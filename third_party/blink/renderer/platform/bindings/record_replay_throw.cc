// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/bindings/record_replay_throw.h"

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/v8_throw_exception.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"

namespace blink {

bool RecordReplayThrowIfEventsUnavailable(const char* operation_name,
                                          ExceptionState* exception_state) {
  if (!recordreplay::AreEventsUnavailable("divergent-side-effect"))
    return false;
  String message = String("Cannot replay operation ") + operation_name +
                   " because it was not recorded.";
  if (exception_state) {
    exception_state->ThrowTypeError(message);
    return true;
  }
  // Without an entered context there is no JS frame to receive the exception,
  // and it would surface at an unrelated later V8 entry.
  v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
  if (isolate && isolate->InContext()) {
    v8::HandleScope handle_scope(isolate);
    V8ThrowException::ThrowTypeError(isolate, message);
  }
  return true;
}

}  // namespace blink
