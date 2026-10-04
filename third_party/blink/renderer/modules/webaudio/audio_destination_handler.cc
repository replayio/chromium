// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webaudio/audio_destination_handler.h"

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/wtf/threading.h"

namespace blink {

AudioDestinationHandler::AudioDestinationHandler(AudioNode& node)
    : AudioHandler(kNodeTypeDestination, node, 0) {
  AddInput();
}

AudioDestinationHandler::~AudioDestinationHandler() {
  DCHECK(!IsInitialized());
}

size_t AudioDestinationHandler::CurrentSampleFrame() const {
  size_t frame = current_sample_frame_.load(std::memory_order_acquire);
  if (IsMainThread()) {
    frame = recordreplay::RecordReplayValue(
        "AudioDestinationHandler::CurrentSampleFrame", frame);
  }
  return frame;
}

}  // namespace blink
