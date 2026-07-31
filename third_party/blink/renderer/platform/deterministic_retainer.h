// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_DETERMINISTIC_RETAINER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_DETERMINISTIC_RETAINER_H_

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_set.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"

namespace recordreplay {

// Off-graph root for Oilpan objects: keep alive past GC-only teardown; drop at
// a deterministic Chromium unused/teardown site.
//
// Lifecycle (leak-references):
//   empty --Retain--> held --Release--> empty
//                 \-------Clear-------/
// No-op when not recording/replaying.
template <typename T>
class DeterministicRetainer {
  DISALLOW_NEW();

 public:
  explicit DeterministicRetainer(const char* label) : label_(label) {}

  void Retain(T* object) {
    if (!object || !IsRecordingOrReplaying("leak-references", label_))
      return;
    EnsureSet().insert(object);
  }

  void Release(T* object) {
    if (!object || !set_)
      return;
    set_->erase(object);
  }

  void Clear() {
    if (set_)
      set_->clear();
  }

 private:
  blink::HeapHashSet<blink::Member<T>>& EnsureSet() {
    if (!set_)
      set_ = blink::MakeGarbageCollected<blink::HeapHashSet<blink::Member<T>>>();
    return *set_;
  }

  const char* label_;
  blink::Persistent<blink::HeapHashSet<blink::Member<T>>> set_;
};

}  // namespace recordreplay

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_DETERMINISTIC_RETAINER_H_
