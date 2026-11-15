// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_WTF_REPLAY_POINTER_ID_HASH_TRAITS_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_WTF_REPLAY_POINTER_ID_HASH_TRAITS_H_

#include "base/check.h"
#include "base/memory/scoped_refptr.h"
#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/wtf/hash_functions.h"
#include "third_party/blink/renderer/platform/wtf/hash_traits.h"

namespace recordreplay {

// Replay intervention: deterministic hashing keyed by the registered
// pointer-id registry. v147 requires KeyTraits to be a full HashTraits, so
// these inherit from the upstream slot-shape parents and only override
// GetHash. See workspaces/planner/rebase-147/ref-pointer-id-hash/plan.md.
inline unsigned GetReplayPointerIdHash(const void* p) {
  if (IsRecordingOrReplaying("pointer-ids")) {
    int id = PointerId(p);
    CHECK(id != 0);
    return HashInt(static_cast<uint32_t>(id));
  }
  return blink::GetHash<const void*>(p);
}

template <typename T>
struct ReplayPointerIdHashTraits : blink::HashTraits<T*> {
  static unsigned GetHash(T* p) { return GetReplayPointerIdHash(p); }
};

template <typename T>
struct ReplayRefPointerIdHashTraits : blink::HashTraits<scoped_refptr<T>> {
  static unsigned GetHash(T* p) { return GetReplayPointerIdHash(p); }
  static unsigned GetHash(const scoped_refptr<T>& p) {
    return GetReplayPointerIdHash(p.get());
  }
};

}  // namespace recordreplay

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_WTF_REPLAY_POINTER_ID_HASH_TRAITS_H_
