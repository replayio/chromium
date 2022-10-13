// Copyright 2024 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <atomic>

#include "base/record_replay.h"
#include "base/record_replay_paint_surface.h"

// We use this file to allow depending on record_replay_render from blink.
// We cannot access the render code directly since, during V8 snapshotting,
// blink is linked, but the display service is not.

namespace recordreplay {

static std::atomic<bool> gPaintSurfaceResetRequested{false};

void DoResetPaintSurface() {
  gPaintSurfaceResetRequested.store(true);
}

bool TakePaintSurfaceReset() {
  bool requested = gPaintSurfaceResetRequested.exchange(false);
  return RecordReplayValue("TakePaintSurfaceReset", requested);
}

}  // namespace recordreplay
