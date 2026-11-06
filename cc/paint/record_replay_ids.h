// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_RECORD_REPLAY_IDS_H_
#define CC_PAINT_RECORD_REPLAY_IDS_H_

#include <limits>

#include "base/atomic_sequence_num.h"
#include "third_party/skia/include/private/SkRecordReplayDraw.h"

namespace cc {

// Code that runs with recorded events disallowed, such as Replay's in-process
// draws while replaying, takes IDs from a range of its own: an ID it took from
// the shared sequence would shift the IDs recorded threads get later. Gated on
// what the driver disallows, as Skia's ID ranges are, and counting down from
// the largest ID as those do.
inline int RecordReplayNextId(base::AtomicSequenceNumber& shared,
                              base::AtomicSequenceNumber& isolated) {
  if (SkRecordReplayAreEventsDisallowedByDriver()) {
    return std::numeric_limits<int>::max() - isolated.GetNext();
  }
  return shared.GetNext();
}

}  // namespace cc

#endif  // CC_PAINT_RECORD_REPLAY_IDS_H_
