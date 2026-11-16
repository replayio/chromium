// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/ref_counted.h"

#include "base/record_replay.h"

#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz::internal {

RefCountedBase::RefCountedBase() = default;

RefCountedBase::~RefCountedBase() = default;

void RefCountedBase::AcquireImpl() {
  ref_count_.fetch_add(1, std::memory_order_relaxed);
}

void RefCountedBase::EnableOrderedRelease(const char* name) {
  // A lock created with events disallowed would get id 0 while recording and
  // behave differently on replay, so skip it in that case; the object then
  // simply keeps the unordered fast path.
  if (recordreplay::IsRecordingOrReplaying() &&
      !recordreplay::AreEventsDisallowed()) {
    record_replay_release_ordered_lock_id_ =
        recordreplay::CreateOrderedLock(name);
  }
}

bool RefCountedBase::ReleaseImpl() {
  // SUBTLE: Technically the load does not need to be an acquire unless we're
  // releasing the last reference and need to delete `this`, but it's not clear
  // whether std::memory_order_acq_rel here will produce more or less efficient
  // code compared to a plain std::memory_order_release followed by an acquire
  // fence in the conditional block below.
  // [Replay] See EnableOrderedRelease.  Serialising the decrement makes the
  // thread that sees the last reference -- and so runs the destructor --
  // reproducible.  The delete itself stays outside the lock, as in M108.
  if (record_replay_release_ordered_lock_id_) {
    recordreplay::AutoOrderedLock ordered(record_replay_release_ordered_lock_id_);
    int last_count = ref_count_.fetch_sub(1, std::memory_order_acq_rel);
    ABSL_ASSERT(last_count > 0);
    return last_count == 1;
  }

  int last_count = ref_count_.fetch_sub(1, std::memory_order_acq_rel);
  ABSL_ASSERT(last_count > 0);
  return last_count == 1;
}

}  // namespace ipcz::internal
