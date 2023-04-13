// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_CONTEXT_LIFECYCLE_OBSERVER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_CONTEXT_LIFECYCLE_OBSERVER_H_

#include "base/dcheck_is_on.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"

#include "third_party/blink/renderer/platform/heap_observer_set.h"

namespace blink {

class ContextLifecycleNotifier;

// Observer that gets notified when the context is destroyed. Used to observe
// ExecutionContext from platform/.
class PLATFORM_EXPORT ContextLifecycleObserver : public GarbageCollectedMixin {
 public:
  virtual ~ContextLifecycleObserver();
  void NotifyContextDestroyed();

  ContextLifecycleNotifier* GetContextLifecycleNotifier() const {
    return notifier_;
  }
  void SetContextLifecycleNotifier(ContextLifecycleNotifier*);

  virtual bool IsExecutionContextLifecycleObserver() const { return false; }

  void Trace(Visitor*) const override;

  int RecordReplayId() const { return recordreplay::PointerId(this); }

 protected:
  ContextLifecycleObserver();

  virtual void ContextDestroyed() = 0;

 private:
  WeakMember<ContextLifecycleNotifier> notifier_;
#if DCHECK_IS_ON()
  bool waiting_for_context_destroyed_ = false;
#endif
};

// RUN-1716
typedef HeapObserverSet<
    ContextLifecycleObserver,
    HeapHashSet<WeakMember<ContextLifecycleObserver>,
                WTF::MemberHashRecordReplayId<ContextLifecycleObserver>>>
    ContextLifecycleHeapObserverSet;

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_CONTEXT_LIFECYCLE_OBSERVER_H_
