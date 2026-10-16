// Copyright 2024 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef BASE_RECORD_REPLAY_PAINT_SURFACE_H_
#define BASE_RECORD_REPLAY_PAINT_SURFACE_H_


namespace recordreplay {

// Asks the compositor thread to pick the paint surface again at its next
// frame. Callable from any thread.
void DoResetPaintSurface();

// Whether a reset was requested since the last call. The compositor thread
// calls this once per submitted frame; the answer is recorded, so a replay
// applies each reset at the same frame as the recording did regardless of
// how the requesting thread interleaves with the compositor thread. After
// diverging from the recording the live flag is used, so a request still
// pending then applies to the first repaint.
bool TakePaintSurfaceReset();

}  // namespace recordreplay

#endif // BASE_RECORD_REPLAY_PAINT_SURFACE_H_
