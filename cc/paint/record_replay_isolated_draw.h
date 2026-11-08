// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_RECORD_REPLAY_ISOLATED_DRAW_H_
#define CC_PAINT_RECORD_REPLAY_ISOLATED_DRAW_H_

#include "base/record_replay.h"
#include "third_party/skia/include/private/SkRecordReplayDraw.h"

namespace cc {

// Code that copies, captures or draws frames in-process when replaying runs
// with recorded events disallowed and in an isolated Skia draw, which takes
// IDs from a range of its own and uses caches of its own.
class RecordReplayScopedIsolatedDraw {
 public:
  explicit RecordReplayScopedIsolatedDraw(const char* why) : disallow_(why) {
    SkRecordReplayBeginIsolatedDraw();
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
    recordreplay::SetInIsolatedDrawForChecks(true);
#endif
  }
  ~RecordReplayScopedIsolatedDraw() {
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
    recordreplay::SetInIsolatedDrawForChecks(false);
#endif
    SkRecordReplayEndIsolatedDraw();
  }

 private:
  recordreplay::AutoDisallowEvents disallow_;
};

}  // namespace cc

#endif  // CC_PAINT_RECORD_REPLAY_ISOLATED_DRAW_H_
