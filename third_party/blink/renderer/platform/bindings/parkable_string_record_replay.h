// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_PARKABLE_STRING_RECORD_REPLAY_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_PARKABLE_STRING_RECORD_REPLAY_H_

#include "third_party/blink/renderer/platform/platform_export.h"

namespace blink {

class ParkableStringImpl;

// Emits a record/replay Assert over the per-string state feeding
// |ParkableStringManager::AgeStringsAndPark()| (digest, status, age,
// HasOneRef). Called outside any lock, before |MaybeAgeOrParkString()|, to
// localize the divergent input behind the `reschedule` DataMismatch.
PLATFORM_EXPORT void AssertParkableStringAgeState(ParkableStringImpl* str);

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_PARKABLE_STRING_RECORD_REPLAY_H_
