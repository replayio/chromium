// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_RECORD_REPLAY_IMAGE_CAPTURE_H_
#define CC_PAINT_RECORD_REPLAY_IMAGE_CAPTURE_H_

#include "cc/paint/paint_export.h"
#include "cc/paint/paint_image.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPixmap.h"

namespace cc {

// When replaying, Replay draws a compositor frame in-process after the frame
// has been sent to the GPU process, and only if the recorder asks for its
// pixels. The images in the frame's filters are captured as the send
// serializes them (PaintOpWriter), so that drawing never decodes images
// itself: decoding fills caches that recorded threads share.
CC_PAINT_EXPORT void RecordReplaySetCapturingImages(bool capturing);
CC_PAINT_EXPORT bool RecordReplayCapturingImages();
// Images are keyed by their stable ID and the content ID of their default
// frame, which is what PaintOpWriter serializes.
CC_PAINT_EXPORT void RecordReplayCaptureImage(const PaintImage& image,
                                              const SkPixmap& pixmap);
CC_PAINT_EXPORT sk_sp<SkImage> RecordReplayFindCapturedImage(
    const PaintImage& image);
CC_PAINT_EXPORT void RecordReplayClearCapturedImages();

}  // namespace cc

#endif  // CC_PAINT_RECORD_REPLAY_IMAGE_CAPTURE_H_
