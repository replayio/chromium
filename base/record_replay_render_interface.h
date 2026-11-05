// Copyright 2024 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef BASE_RECORD_REPLAY_RENDER_INTERFACE_H_
#define BASE_RECORD_REPLAY_RENDER_INTERFACE_H_


#include "ui/gfx/geometry/size.h"

using gfx::Size;

namespace recordreplay {

typedef gfx::Size (*GetCurrentViewportPixelSizeCallback)();
// This callback is set when the first render surface is created.
void SetGetCurrentViewportPixelSizeCallback(GetCurrentViewportPixelSizeCallback cb);
// Get the current surface size in pixels.
gfx::Size GetCurrentViewportPixelSize();

// Whether the compositor drew the last repaint with the frame sink repaints
// give it when it waits for one.
void SetLastRepaintUsedRepaintFrameSink(bool used);
bool LastRepaintUsedRepaintFrameSink();


}  // namespace recordreplay

#endif // BASE_RECORD_REPLAY_RENDER_INTERFACE_H_
