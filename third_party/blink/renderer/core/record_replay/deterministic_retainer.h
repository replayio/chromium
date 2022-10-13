// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_RECORD_REPLAY_DETERMINISTIC_RETAINER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_RECORD_REPLAY_DETERMINISTIC_RETAINER_H_

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_set.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"

namespace recordreplay {

// Off-graph Oilpan root (leak-references).
// Retain before GC-controlled cleanup side-effects.
// Release where that cleanup cannot reintroduce the divergence.
//
// Lifecycle: empty --Retain--> held --Release|Clear--> empty
template <typename T>
class DeterministicRetainer {
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

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_RECORD_REPLAY_DETERMINISTIC_RETAINER_H_
