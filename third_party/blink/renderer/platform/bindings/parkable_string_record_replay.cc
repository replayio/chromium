// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/bindings/parkable_string_record_replay.h"

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/parkable_string.h"

namespace blink {

void AssertParkableStringAgeState(ParkableStringImpl* str) {
  ParkableStringImpl::AgeStateSnapshot snapshot = str->CaptureAgeStateSnapshot();

  // Hex-encode the digest so the call site stays a single Assert line.
  char digest_hex[2 * ParkableStringImpl::kDigestSize + 1];
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < snapshot.digest.size(); ++i) {
    digest_hex[2 * i] = kHex[(snapshot.digest[i] >> 4) & 0xf];
    digest_hex[2 * i + 1] = kHex[snapshot.digest[i] & 0xf];
  }
  digest_hex[2 * snapshot.digest.size()] = '\0';

  recordreplay::Assert(
      "ParkableStringManager::AgeStringsAndPark str digest=%s status=%d "
      "age=%d hasOneRef=%d",
      digest_hex, snapshot.status, snapshot.age, snapshot.has_one_ref);
}

}  // namespace blink
