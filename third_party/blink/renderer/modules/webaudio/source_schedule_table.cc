// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webaudio/source_schedule_table.h"

#include "third_party/blink/renderer/modules/webaudio/audio_scheduled_source_handler.h"
#include "third_party/blink/renderer/platform/wtf/threading.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

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

void SourceScheduleTable::Clear() {
  DCHECK(IsMainThread());
  rows_.clear();
}

void SourceScheduleTable::FireDues(size_t fake_audio_clock) {
  DCHECK(IsMainThread());

  // Retire under DueRule first, then fire outside the map walk. NotifyEnded
  // may re-enter Start/Stop and mutate rows_. Hold refs across fire so
  // FinishSourceOnMainThread / close cannot UAF peer dues.
  Vector<scoped_refptr<AudioScheduledSourceHandler>> start_due;
  Vector<scoped_refptr<AudioScheduledSourceHandler>> stop_due;
  Vector<AudioScheduledSourceHandler*> stale;

  for (auto& entry : rows_) {
    AudioScheduledSourceHandler* source = entry.key;
    Row& row = entry.value;

    if (row.start_live && row.start_bound <= fake_audio_clock) {
      row.start_live = false;
      start_due.push_back(source);
    }

    if (row.stop_live && row.stop_bound <= fake_audio_clock) {
      row.stop_live = false;
      if (!row.ended_latched) {
        row.ended_latched = true;
        stop_due.push_back(source);
      }
    }

    if (!row.start_live && !row.stop_live) {
      stale.push_back(source);
    }
  }

  for (AudioScheduledSourceHandler* source : stale) {
    rows_.erase(source);
  }

  for (auto& source : start_due) {
    source->FireStartDue();
  }
  for (auto& source : stop_due) {
    source->FireEndedDue();
  }
}

}  // namespace blink
