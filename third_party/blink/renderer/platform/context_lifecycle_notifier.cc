// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/context_lifecycle_notifier.h"

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/context_lifecycle_observer.h"

namespace blink {

ContextLifecycleNotifier::~ContextLifecycleNotifier() {
#if DCHECK_IS_ON()
  // `NotifyContextDestroyed()` must be called prior to destruction.
  DCHECK(did_notify_observers_);
#endif
}

void ContextLifecycleNotifier::AddContextLifecycleObserver(
    ContextLifecycleObserver* observer) {
  observers_.AddObserver(observer);

  if (recordreplay::IsRecordingOrReplaying("values") && recordreplay::IsReplaying())
    replay_observers_.push_back(observer);
}

void ContextLifecycleNotifier::RemoveContextLifecycleObserver(
    ContextLifecycleObserver* observer) {
  DCHECK(observers_.HasObserver(observer));
  observers_.RemoveObserver(observer);

  for (auto it = replay_observers_.begin(); it != replay_observers_.end(); ++it) {
    if (*it == observer) {
      replay_observers_.erase(it);
      break;
    }
  }
}

void ContextLifecycleNotifier::NotifyContextDestroyed() {
  HeapVector<Member<ContextLifecycleObserver>> observers;
  observers_.ForEachObserver([&](ContextLifecycleObserver* observer) {
    observers.push_back(observer);
  });
  observers_.Clear();

  if (recordreplay::IsRecordingOrReplaying("values") &&
      !recordreplay::AreEventsDisallowed()) {
    if (recordreplay::IsRecording()) {
      size_t num_observers = observers.size();
      recordreplay::RecordReplayValue("NotifyContextDestroyed NumObservers", num_observers);
      int* observer_ids = new int[num_observers];
      for (size_t i = 0; i < observers.size(); i++) {
        int id = recordreplay::PointerId(observers[i]);
        CHECK(id);
        observer_ids[i] = id;
      }
      recordreplay::RecordReplayBytes("ContextLifecycleNotifier::NotifyContextDestroyed ObserverIds",
                                      observer_ids, num_observers * sizeof(int));
      delete[] observer_ids;
    } else {
      size_t num_observers = recordreplay::RecordReplayValue("NotifyContextDestroyed NumObservers", 0);
      int* observer_ids = new int[num_observers];
      recordreplay::RecordReplayBytes("ContextLifecycleNotifier::NotifyContextDestroyed ObserverIds",
                                      observer_ids, num_observers * sizeof(int));

      HeapVector<Member<ContextLifecycleObserver>> new_observers;
      for (ContextLifecycleObserver* observer : observers) {
        int id = recordreplay::PointerId(observer);
        CHECK(id);
        bool found = false;
        for (size_t i = 0; i < num_observers; i++) {
          if (observer_ids[i] == id) {
            found = true;
            break;
          }
        }
        if (found)
          new_observers.push_back(observer);
      }

      observers = std::move(new_observers);
      delete[] observer_ids;
    }
  }

  for (ContextLifecycleObserver* observer : observers) {
    if (!recordreplay::AreEventsDisallowed()) {
      recordreplay::Assert("ContextLifecycleNotifier::NotifyContextDestroyed #1 %d",
                           recordreplay::PointerId(observer));
    }
    observer->NotifyContextDestroyed();
  }

  replay_observers_.clear();

#if DCHECK_IS_ON()
  did_notify_observers_ = true;
#endif
}

void ContextLifecycleNotifier::Trace(Visitor* visitor) const {
  visitor->Trace(observers_);
  visitor->Trace(replay_observers_);
}

}  // namespace blink
