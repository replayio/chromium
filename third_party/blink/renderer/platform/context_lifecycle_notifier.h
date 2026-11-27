// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_CONTEXT_LIFECYCLE_NOTIFIER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_CONTEXT_LIFECYCLE_NOTIFIER_H_

#include <algorithm>

#include "base/dcheck_is_on.h"
#include "base/functional/callback.h"
#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap_observer_list.h"

#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_map.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_set.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_vector.h"

namespace v8 {
class Isolate;
}  // namespace v8

namespace blink {

class ContextLifecycleObserver;
class RecordReplayObserverCollection;

// Notifier interface for ContextLifecycleObserver.
class PLATFORM_EXPORT ContextLifecycleNotifier : public GarbageCollectedMixin {
 public:
  virtual ~ContextLifecycleNotifier();

  void AddContextLifecycleObserver(ContextLifecycleObserver*);
  void RemoveContextLifecycleObserver(ContextLifecycleObserver*);

  void Trace(Visitor* visitor) const override;

  bool IsContextDestroyed() const;

  // Observers are kept alive while recording or replaying, as the list holds
  // them weakly. This lets the GC collect one which nobody removes, so it does
  // not leak until the context is destroyed. `release` runs on both sides at
  // the GC poll after the recording collected the observer, and should undo
  // what other weak holders of it would see (see RecordReplayGCRelease).
  void RecordReplayLetCollect(v8::Isolate* isolate,
                              ContextLifecycleObserver* observer,
                              base::OnceClosure release);

 protected:
  // Should be called by implementers to notify observers when the context is
  // destroyed.
  void NotifyContextDestroyed();

  const HeapObserverList<ContextLifecycleObserver>& observers() const {
    return observers_;
  }
  HeapObserverList<ContextLifecycleObserver>& observers() { return observers_; }

  // Iterates like observers().ForEachObserver(), except while recording or
  // replaying. Observers are then notified in pointer id order, and the replay
  // notifies only the ones the recording did (see RecordReplayFilterObservers).
  // The callbacks run over a copy, so an observer may remove itself, and one
  // which an earlier callback removed is skipped.
  template <typename ForEachCallable>
  void RecordReplayForEachObserver(const char* label,
                                   const ForEachCallable& callable) {
    if (!recordreplay::IsRecordingOrReplaying()) {
      observers_.ForEachObserver(callable);
      return;
    }
    HeapVector<Member<ContextLifecycleObserver>> observers;
    observers_.ForEachObserver([&](ContextLifecycleObserver* observer) {
      observers.push_back(observer);
    });
    std::sort(observers.begin(), observers.end(),
              recordreplay::CompareMemberByPointerId<
                  Member<ContextLifecycleObserver>>());
    RecordReplayFilterObservers(label, observers);
    for (ContextLifecycleObserver* observer : observers) {
      if (observers_.HasObserver(observer)) {
        callable(observer);
      }
    }
  }

 private:
  static void RecordReplayFilterObservers(
      const char* label,
      HeapVector<Member<ContextLifecycleObserver>>& observers);

  HeapObserverList<ContextLifecycleObserver> observers_;
  bool context_destroyed_ = false;

  // When replaying, strong references are held on all observers, which are
  // cleared out after the context is destroyed. This ensures we can notify
  // the same observers when replaying as when originally recording
  HeapHashSet<Member<ContextLifecycleObserver>> replay_observers_;
  // Notes the collection of an observer for RecordReplayLetCollect(). A value
  // dies with its key, or with this notifier when that is collected first.
  HeapHashMap<WeakMember<ContextLifecycleObserver>,
              Member<RecordReplayObserverCollection>>
      replay_observer_collections_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_CONTEXT_LIFECYCLE_NOTIFIER_H_
