// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/paint/record_replay_image_capture.h"

#include <map>
#include <utility>

#include "base/no_destructor.h"
#include "base/record_replay.h"
#include "third_party/skia/include/private/SkRecordReplayDraw.h"

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
  // from the range for isolated draws.
  recordreplay::AutoDisallowEvents disallow("RecordReplayCaptureImage");
  struct ScopedIsolatedDraw {
    ScopedIsolatedDraw() {
      SkRecordReplayBeginIsolatedDraw();
    }
    ~ScopedIsolatedDraw() {
      SkRecordReplayEndIsolatedDraw();
    }
  } isolated;
  CapturedImage& captured = CapturedImages()[KeyFor(image)];
  captured.captured = true;
  // Frames mostly send the images the previous frame sent. The key's content
  // ID changes with the pixels of the frame PaintOpWriter serializes (the
  // default one), so the earlier copy has the same pixels: keeping it saves
  // copying them again, and draws find what they cached for the image.
  if (captured.image) {
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
