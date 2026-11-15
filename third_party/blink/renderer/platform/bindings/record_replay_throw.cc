// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/bindings/record_replay_throw.h"

#include <tuple>

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/v8_throw_exception.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

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
    // Thrown with this message and nothing before it, as in Replay's Chromium
    // 108. This Chromium names the API an exception leaves through ("Failed to
    // execute 'appendChild' on 'Node': ...") as it propagates; the mark has
    // V8Initializer::ExceptionPropagationCallback leave it be.
    v8::Local<v8::Value> exception =
        V8ThrowException::CreateTypeError(isolate, message);
    if (exception->IsObject()) {
      std::ignore = exception.As<v8::Object>()->SetPrivate(
          isolate->GetCurrentContext(),
          v8::Private::ForApi(isolate,
                              v8::String::NewFromUtf8Literal(
                                  isolate, kRecordReplayPlainMessageMark)),
          v8::True(isolate));
    }
    isolate->ThrowException(exception);
  }
  return true;
}

}  // namespace blink
