// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/fonts/shaping/harfbuzz_font_cache.h"

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/fonts/shaping/harfbuzz_face.h"
#include "third_party/blink/renderer/platform/fonts/shaping/harfbuzz_font_data.h"

namespace blink {

void HarfBuzzFontCache::Trace(Visitor* visitor) const {
  visitor->Trace(font_map_);
  visitor->Trace(record_replay_held_);
}

void HarfBuzzFontCache::RecordReplayHold(uint64_t unique_id,
                                         HarfBuzzFontData* font_data) {
  if (!recordreplay::IsRecordingOrReplaying()) {
    return;
  }
  // See the header.
  static const wtf_size_t kMaxHeld = 1024;
  record_replay_held_.push_back(font_data);
  record_replay_keys_.push_back(unique_id);
  while (record_replay_held_.size() > kMaxHeld) {
    auto it = font_map_.find(record_replay_keys_.front());
    if (it != font_map_.end() && it->value == record_replay_held_.front()) {
      font_map_.erase(it);
    }
    record_replay_held_.pop_front();
    record_replay_keys_.pop_front();
  }
}

}  // namespace blink
