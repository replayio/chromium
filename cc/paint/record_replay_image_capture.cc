// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/paint/record_replay_image_capture.h"

#include <map>
#include <utility>

#include "base/no_destructor.h"
#include "base/record_replay.h"

namespace cc {

namespace {

// Only used on the compositor thread, which sends and draws frames.
bool g_capturing_images = false;

using CapturedImageKey = std::pair<PaintImage::Id, PaintImage::ContentId>;

std::map<CapturedImageKey, sk_sp<SkImage>>& CapturedImages() {
  static base::NoDestructor<std::map<CapturedImageKey, sk_sp<SkImage>>>
      images;
  return *images;
}

CapturedImageKey KeyFor(const PaintImage& image) {
  return {image.stable_id(),
          image.GetContentIdForFrame(PaintImage::kDefaultFrameIndex)};
}

}  // namespace

void RecordReplaySetCapturingImages(bool capturing) {
  g_capturing_images = capturing;
}

bool RecordReplayCapturingImages() {
  return g_capturing_images;
}

void RecordReplayCaptureImage(const PaintImage& image,
                              const SkPixmap& pixmap) {
  // Copying the pixels emits no recorded events of its own; new images get IDs
  // from the range for code with events disallowed.
  recordreplay::AutoDisallowEvents disallow("RecordReplayCaptureImage");
  sk_sp<SkImage> copy = SkImage::MakeRasterCopy(pixmap);
  if (copy) {
    CapturedImages()[KeyFor(image)] = std::move(copy);
  }
}

sk_sp<SkImage> RecordReplayFindCapturedImage(const PaintImage& image) {
  auto it = CapturedImages().find(KeyFor(image));
  return it == CapturedImages().end() ? nullptr : it->second;
}

void RecordReplayClearCapturedImages() {
  CapturedImages().clear();
}

}  // namespace cc
