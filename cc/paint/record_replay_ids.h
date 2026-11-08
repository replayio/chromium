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

// Replay's isolated draws (SkRecordReplayBeginIsolatedDraw) take IDs from a
// range of their own: an ID they took from the shared sequence would shift the
// IDs recorded threads get later. Counts down from the largest ID, as Skia's
// isolated IDs do.
inline int RecordReplayNextId(base::AtomicSequenceNumber& shared,
                              base::AtomicSequenceNumber& isolated) {
  if (SkRecordReplayInIsolatedDraw()) {
    return std::numeric_limits<int>::max() - isolated.GetNext();
  }
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
  if (recordreplay::AreEventsDisallowed()) {
    recordreplay::IsolatedDrawCheckAlways("cc ID with events disallowed",
                                          __builtin_return_address(0));
  }
#endif
  return shared.GetNext();
}

}  // namespace cc

#endif  // CC_PAINT_RECORD_REPLAY_IDS_H_
