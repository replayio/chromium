// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/bindings/record_replay_throw.h"

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/v8_throw_exception.h"
#include "v8/include/v8-isolate.h"

namespace blink {

const char kReplayUnavailableNetworkMessage[] =
    "This evaluation tried to read network contents that were not captured in "
    "the recording. Replay cannot perform fresh network reads during replay.";

const char kReplayUnavailableBlobMessage[] =
    "This evaluation tried to read file or blob contents that were not "
    "captured in the recording. Replay cannot perform fresh file/blob reads "
    "during replay.";

const char kReplayUnavailableCookieMessage[] =
    "This evaluation tried to access cookies that require IPC not available "
    "during replay. Replay cannot perform fresh cookie reads or writes during "
    "replay.";

const char kReplayUnavailableStorageMessage[] =
    "This evaluation tried to access web storage that requires IPC not "
    "available during replay. Replay cannot perform fresh storage reads or "
    "writes during replay.";

const char kReplayUnavailableScrollMessage[] =
    "This evaluation tried to programmatically scroll. Replay cannot perform "
    "compositor scroll commits during replay.";

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
  if (v8::Isolate* isolate = v8::Isolate::GetCurrent())
    V8ThrowException::ThrowTypeError(isolate, message);
  return true;
}

}  // namespace blink
