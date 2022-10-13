// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/bindings/parkable_string_record_replay.h"

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/parkable_string.h"

namespace blink {

void AssertParkableStringAgeState(ParkableStringImpl* str) {
  ParkableStringImpl::AgeStateSnapshot snapshot = str->CaptureAgeStateSnapshot();

  recordreplay::Assert(
      "ParkableStringManager::AgeStringsAndPark str status=%d age=%d "
      "hasOneRef=%d",
      snapshot.status, snapshot.age, snapshot.has_one_ref);
}

}  // namespace blink
