// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_RECORD_REPLAY_THROW_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_RECORD_REPLAY_THROW_H_

#include "third_party/blink/renderer/platform/platform_export.h"

namespace blink {

class ExceptionState;

// If events are unavailable: throw `message` and return true (caller returns).
// Without an `exception_state` the throw needs an entered V8 context; the
// `true` return bails out either way.
PLATFORM_EXPORT bool RecordReplayThrowIfEventsUnavailable(
    const char* message,
    ExceptionState* exception_state = nullptr);

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_RECORD_REPLAY_THROW_H_
