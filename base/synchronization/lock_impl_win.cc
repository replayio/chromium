// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/synchronization/lock_impl.h"

#include "base/debug/activity_tracker.h"

#include <windows.h>

namespace base {
namespace internal {

extern "C" void V8RecordReplayAddOrderedSRWLock(const char* name, void* aLock);

LockImpl::LockImpl(const char* ordered_name) : native_handle_(SRWLOCK_INIT) {
  if (ordered_name)
    V8RecordReplayAddOrderedSRWLock(ordered_name, &native_handle_);
}

LockImpl::~LockImpl() = default;

void LockImpl::LockInternalWithTracking() {
  base::debug::ScopedLockAcquireActivity lock_activity(this);
  ::AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&native_handle_));
}

}  // namespace internal
}  // namespace base
