// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webaudio/source_schedule_table.h"

#include "third_party/blink/renderer/modules/webaudio/audio_scheduled_source_handler.h"
#include "third_party/blink/renderer/platform/wtf/threading.h"

namespace blink {

void SourceScheduleTable::InsertStart(AudioScheduledSourceHandler* source,
                                      size_t start_bound) {
  DCHECK(IsMainThread());
  DCHECK(source);
  Row& row = rows_.insert(source, Row()).stored_value->value;
  row.start_bound = start_bound;
  row.start_live = true;
}

void SourceScheduleTable::InsertOrSupersedeStop(
    AudioScheduledSourceHandler* source,
    size_t stop_bound) {
  DCHECK(IsMainThread());
  DCHECK(source);
  Row& row = rows_.insert(source, Row()).stored_value->value;
  // Stop supersede: retire prior live stop binder without fire.
  row.stop_bound = stop_bound;
  row.stop_live = true;
}

void SourceScheduleTable::FireDues(size_t fake_audio_clock) {
  DCHECK(IsMainThread());

  Vector<AudioScheduledSourceHandler*> stale;
  for (auto& entry : rows_) {
    AudioScheduledSourceHandler* source = entry.key;
    Row& row = entry.value;

    if (row.start_live && row.start_bound <= fake_audio_clock) {
      source->FireStartDue();
      row.start_live = false;
    }

    if (row.stop_live && row.stop_bound <= fake_audio_clock) {
      if (!row.ended_latched) {
        row.ended_latched = true;
        source->FireEndedDue();
      }
      row.stop_live = false;
    }

    if (!row.start_live && !row.stop_live) {
      stale.push_back(source);
    }
  }

  for (AudioScheduledSourceHandler* source : stale) {
    rows_.erase(source);
  }
}

}  // namespace blink
