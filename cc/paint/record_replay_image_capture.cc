// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/paint/record_replay_image_capture.h"

#include <string.h>

#include <map>
#include <utility>

#include "base/no_destructor.h"
#include "base/record_replay.h"

namespace cc {

namespace {

// Only used on the compositor thread, which sends frames
// (AsyncLayerTreeFrameSink::SubmitCompositorFrame serializes them) and draws
// them, so nothing here is locked.
bool g_capturing_images = false;

using CapturedImageKey = std::pair<PaintImage::Id, PaintImage::ContentId>;

struct CapturedImage {
  sk_sp<SkImage> image;
  // Whether the frame being sent captured the image.
  bool captured = false;
};

std::map<CapturedImageKey, CapturedImage>& CapturedImages() {
  static base::NoDestructor<std::map<CapturedImageKey, CapturedImage>> images;
  return *images;
}

bool HasPixels(const sk_sp<SkImage>& image, const SkPixmap& pixmap) {
  SkPixmap pixels;
  if (!image->peekPixels(&pixels) || pixels.info() != pixmap.info()) {
    return false;
  }
  size_t row_bytes = pixmap.info().minRowBytes();
  for (int y = 0; y < pixmap.height(); y++) {
    if (memcmp(pixels.addr(0, y), pixmap.addr(0, y), row_bytes)) {
      return false;
    }
  }
  return true;
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
  CapturedImage& captured = CapturedImages()[KeyFor(image)];
  captured.captured = true;
  // Frames mostly send the images the previous frame sent. Keeping the copy
  // of the same pixels saves copying them again, and draws find what they
  // cached for the image.
  if (captured.image && HasPixels(captured.image, pixmap)) {
    return;
  }
  captured.image = SkImage::MakeRasterCopy(pixmap);
}

sk_sp<SkImage> RecordReplayFindCapturedImage(const PaintImage& image) {
  // A capture that the frame being drawn didn't send is the previous frame's.
  auto it = CapturedImages().find(KeyFor(image));
  if (it == CapturedImages().end() || !it->second.captured) {
    return nullptr;
  }
  return it->second.image;
}

void RecordReplayClearCapturedImages() {
  auto& images = CapturedImages();
  for (auto it = images.begin(); it != images.end();) {
    if (it->second.captured && it->second.image) {
      it->second.captured = false;
      ++it;
    } else {
      it = images.erase(it);
    }
  }
}

}  // namespace cc
