// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_WEBAUDIO_SOURCE_SCHEDULE_TABLE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_WEBAUDIO_SOURCE_SCHEDULE_TABLE_H_

#include <cstddef>

#include "base/memory/scoped_refptr.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"

namespace blink {

class AudioScheduledSourceHandler;

// Sole due-ness store for MainThreadSubstitute. Writers = main-thread
// start/stop only; AT must not insert. Eval only on QuantumEdge MainThreadTask
// under DueRule (bound <= FakeAudioClock) + RetireRule (one-shot).
class MODULES_EXPORT SourceScheduleTable {
  DISALLOW_NEW();

 public:
  SourceScheduleTable() = default;

  void InsertStart(AudioScheduledSourceHandler* source, size_t start_bound);
  // Retires any prior live stop binder without fire (stop-supersede).
  void InsertOrSupersedeStop(AudioScheduledSourceHandler* source,
                             size_t stop_bound);

  // DueRule + RetireRule. Must run on main.
  void FireDues(size_t fake_audio_clock);

 private:
  struct Row {
    size_t start_bound = 0;
    size_t stop_bound = 0;
    bool start_live = false;
    bool stop_live = false;
    bool ended_latched = false;
  };

  HashMap<AudioScheduledSourceHandler*, Row> rows_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_WEBAUDIO_SOURCE_SCHEDULE_TABLE_H_
