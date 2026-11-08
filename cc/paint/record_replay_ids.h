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

// Code that runs with recorded events disallowed, isolated draws among it,
// takes IDs from a range of its own: an ID it took from the shared sequence
// would shift the IDs recorded threads get later, and some of that code
// (replay-only commands, unordered tasks) doesn't run at the same points when
// recording and replaying. Counts down from the largest ID, as Skia's do.
inline int RecordReplayNextId(base::AtomicSequenceNumber& shared,
                              base::AtomicSequenceNumber& isolated) {
  if (SkRecordReplayAreEventsDisallowedByDriver()) {
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
    if (!SkRecordReplayInIsolatedDraw()) {
      recordreplay::IsolatedDrawCheckAlways(
          "cc ID outside an isolated draw", __builtin_return_address(0));
    }
#endif
    return std::numeric_limits<int>::max() - isolated.GetNext();
  }
  return shared.GetNext();
}

}  // namespace cc

#endif  // CC_PAINT_RECORD_REPLAY_IDS_H_
