// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_RECORD_REPLAY_IDS_H_
#define CC_PAINT_RECORD_REPLAY_IDS_H_

#include <atomic>
#include <limits>

#include "base/atomic_sequence_num.h"
#include "third_party/skia/include/private/SkRecordReplayDraw.h"

#include "base/record_replay_isolated_draw_asserts.h"

namespace cc {

// Code that runs with recorded events disallowed takes IDs from ranges of its
// own: an ID it took from the shared sequence would shift the IDs recorded
// threads get later, and some of that code (replay-only commands, unordered
// tasks) doesn't run at the same points when recording and replaying.
// Isolated draws, which run only when replaying, have a range apart from the
// rest of that code. Both count down, as Skia's do.
inline int RecordReplayNextId(base::AtomicSequenceNumber& shared,
                              base::AtomicSequenceNumber& isolated) {
  if (SkRecordReplayAreEventsDisallowedByDriver()) {
    if (!SkRecordReplayInIsolatedDraw()) {
      recordreplay::IsolatedDrawNote(
          "cc ID outside an isolated draw", __builtin_return_address(0));
    }
    if (SkRecordReplayInIsolatedDraw()) {
      // Only isolated draws use this counter, so it isn't an
      // AtomicSequenceNumber, which RECORD_REPLAY_ISOLATED_DRAW_ASSERTS
      // fails on in an isolated draw.
      static std::atomic<int> draw{0};
      return std::numeric_limits<int>::max() -
             draw.fetch_add(1, std::memory_order_relaxed);
    }
    return std::numeric_limits<int>::max() / 2 - isolated.GetNext();
  }
  return shared.GetNext();
}

}  // namespace cc

#endif  // CC_PAINT_RECORD_REPLAY_IDS_H_
