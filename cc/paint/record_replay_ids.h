// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_RECORD_REPLAY_IDS_H_
#define CC_PAINT_RECORD_REPLAY_IDS_H_

#include <limits>

#include "base/atomic_sequence_num.h"
#include "third_party/skia/include/private/SkRecordReplayDraw.h"

#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
#include "base/record_replay.h"
#endif

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
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
    if (!SkRecordReplayInIsolatedDraw()) {
      recordreplay::IsolatedDrawCheckAlways(
          "cc ID outside an isolated draw", __builtin_return_address(0));
    }
#endif
    if (SkRecordReplayInIsolatedDraw()) {
      static base::AtomicSequenceNumber draw;
      return std::numeric_limits<int>::max() - draw.GetNext();
    }
    return std::numeric_limits<int>::max() / 2 - isolated.GetNext();
  }
  return shared.GetNext();
}

}  // namespace cc

#endif  // CC_PAINT_RECORD_REPLAY_IDS_H_
