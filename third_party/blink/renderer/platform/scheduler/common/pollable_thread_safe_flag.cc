// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/scheduler/common/pollable_thread_safe_flag.h"

#include <atomic>

#include "base/record_replay.h"

PollableThreadSafeFlag::PollableThreadSafeFlag(base::Lock* write_lock_, const char* ordered_name)
  : ordered_lock_id_(0), flag_(false), write_lock_(write_lock_)
{
  if (ordered_name) {
    ordered_lock_id_ = recordreplay::CreateOrderedLock(ordered_name);
  }
}

void PollableThreadSafeFlag::SetWhileLocked(bool value) {
  recordreplay::AutoOrderedLock ordered(ordered_lock_id_);
  write_lock_->AssertAcquired();
  flag_.store(value, std::memory_order_release);
}

bool PollableThreadSafeFlag::IsSet() const {
  recordreplay::AutoOrderedLock ordered(ordered_lock_id_);
  return flag_.load(std::memory_order_acquire);
}
