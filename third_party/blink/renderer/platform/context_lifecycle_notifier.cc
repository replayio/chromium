// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/context_lifecycle_notifier.h"

#include <algorithm>

#include "third_party/blink/renderer/platform/bindings/record_replay_gc_release.h"
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#include "third_party/blink/renderer/platform/context_lifecycle_observer.h"
#include "third_party/blink/renderer/platform/heap/prefinalizer.h"

#include <vector>

#include "base/record_replay.h"

namespace blink {

ContextLifecycleNotifier::~ContextLifecycleNotifier() {
  // `NotifyContextDestroyed()` must be called prior to destruction.
  DCHECK(context_destroyed_);
}

bool ContextLifecycleNotifier::IsContextDestroyed() const {
  return context_destroyed_;
}

void ContextLifecycleNotifier::AddContextLifecycleObserver(
    ContextLifecycleObserver* observer) {
  observers_.AddObserver(observer);

  if (recordreplay::IsRecordingOrReplaying("values") && recordreplay::IsReplaying())
    replay_observers_.insert(observer);
}

void ContextLifecycleNotifier::RemoveContextLifecycleObserver(
    ContextLifecycleObserver* observer) {
  DCHECK(observers_.HasObserver(observer));
  observers_.RemoveObserver(observer);

  replay_observers_.erase(observer);
}

class RecordReplayObserverCollection final
    : public GarbageCollected<RecordReplayObserverCollection> {
  USING_PRE_FINALIZER(RecordReplayObserverCollection, Dispose);

 public:
  void Dispose() { RecordReplayGCRelease::OnCollected(release_id_); }
  void Trace(Visitor*) const {}

  int release_id_ = 0;
};

void ContextLifecycleNotifier::RecordReplayLetCollect(
    v8::Isolate* isolate,
    ContextLifecycleObserver* observer,
    base::OnceClosure release) {
  if (!recordreplay::IsRecordingOrReplaying("leak-references",
                                            "HeapObserverList")) {
    return;
  }
  auto* collection = MakeGarbageCollected<RecordReplayObserverCollection>();
  // The replay keeps the observer alive until the release runs, and filters it
  // out of the notifications in the meantime (see RecordReplayFilterObservers).
  collection->release_id_ =
      RecordReplayGCRelease::Track(isolate, observer, std::move(release));
  if (!collection->release_id_) {
    return;
  }
  observers_.RecordReplayLetCollect(observer);
  replay_observers_.erase(observer);
  replay_observer_collections_.Set(observer, collection);
}

void ContextLifecycleNotifier::NotifyContextDestroyed() {
  context_destroyed_ = true;

  ScriptForbiddenScope forbid_script;
  HeapVector<Member<ContextLifecycleObserver>> observers;
  observers_.ForEachObserver([&](ContextLifecycleObserver* observer) {
    observers.push_back(observer);
  });
  observers_.Clear();

  std::sort(observers.begin(), observers.end(),
            recordreplay::CompareMemberByPointerId<Member<ContextLifecycleObserver>>());

  RecordReplayFilterObservers("ContextLifecycleNotifier::NotifyContextDestroyed",
                              observers);

  for (ContextLifecycleObserver* observer : observers) {
    if (!recordreplay::AreEventsDisallowed()) {
      recordreplay::Assert("ContextLifecycleNotifier::NotifyContextDestroyed #1 %d",
                           recordreplay::PointerId(observer));
    }
    observer->NotifyContextDestroyed();
  }

  replay_observers_.clear();
}

// static
void ContextLifecycleNotifier::RecordReplayFilterObservers(
    const char* label,
    HeapVector<Member<ContextLifecycleObserver>>& observers) {
  // When replaying, notify the same observers which were notified when
  // recording. The list holds its observers weakly, so its contents vary with
  // the GC. When replaying, replay_observers_ and RecordReplayLetCollect() keep
  // alive every observer the recording still has, so none of the ones to
  // notify are collected.
  if (recordreplay::IsRecordingOrReplaying("values", label) &&
      recordreplay::FeatureEnabled("pointer-ids") &&
      !recordreplay::AreEventsDisallowed()) {
    size_t num_observers = recordreplay::RecordReplayValue(label, observers.size());
    std::vector<int> observer_ids(num_observers);

    if (recordreplay::IsRecording()) {
      for (wtf_size_t i = 0; i < observers.size(); i++) {
        int id = recordreplay::PointerId(observers[i]);
        CHECK(id);
        observer_ids[i] = id;
      }
    }

    recordreplay::RecordReplayBytes(label,
                                    observer_ids.data(), num_observers * sizeof(int));

    if (recordreplay::IsReplaying()) {
      // There can be hundreds of thousands of observers, too many for a scan
      // per observer.
      std::sort(observer_ids.begin(), observer_ids.end());
      HeapVector<Member<ContextLifecycleObserver>> new_observers;
      for (ContextLifecycleObserver* observer : observers) {
        int id = recordreplay::PointerId(observer);
        CHECK(id);
        if (std::binary_search(observer_ids.begin(), observer_ids.end(), id))
          new_observers.push_back(observer);
      }

      observers = std::move(new_observers);
    }

  }
}

void ContextLifecycleNotifier::Trace(Visitor* visitor) const {
  visitor->Trace(observers_);
  visitor->Trace(replay_observers_);
  visitor->Trace(replay_observer_collections_);
}

}  // namespace blink
