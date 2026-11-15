// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_ORDERED_MUTEX_H_
#define IPCZ_SRC_UTIL_ORDERED_MUTEX_H_

#include "third_party/abseil-cpp/absl/base/thread_annotations.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/record_replay.h"

namespace ipcz {

// An absl::Mutex which threads acquire in the same order when replaying as
// they did when recording.
//
// ipcz state is shared between the thread which receives from a transport and
// the threads which use portals, and what each of them does next depends on
// which one got to the state first. absl::Mutex has no ordering support in the
// record/replay driver, so every acquisition is wrapped in one of the driver's
// ordered locks. This takes the place of the ordered base::Locks of the legacy
// Mojo core (ports::Port, ports::Node, NodeController, MessagePipeDispatcher).
//
// Acquisitions made while events are disallowed or passed through run at
// non-deterministic points and are not ordered, like the legacy core's
// AutoLockMaybeEventsDisallowed. Outside of a recording this is a plain
// absl::Mutex.
class ABSL_LOCKABLE OrderedMutex {
 public:
  explicit OrderedMutex(const char* name)
      : ordered_lock_id_(replay::CreateOrderedLock(name)) {}
  OrderedMutex(const OrderedMutex&) = delete;
  OrderedMutex& operator=(const OrderedMutex&) = delete;

  void Lock() ABSL_EXCLUSIVE_LOCK_FUNCTION() {
    const bool ordered = ordered_lock_id_ && replay::IsRecordable();
    if (ordered) {
      ::recordreplay::OrderedLock(ordered_lock_id_);
    }
    mutex_.Lock();
    holds_ordered_lock_ = ordered;
  }

  void Unlock() ABSL_UNLOCK_FUNCTION() {
    const bool ordered = holds_ordered_lock_;
    holds_ordered_lock_ = false;
    mutex_.Unlock();
    if (ordered) {
      ::recordreplay::OrderedUnlock(ordered_lock_id_);
    }
  }

  void AssertHeld() const ABSL_ASSERT_EXCLUSIVE_LOCK() { mutex_.AssertHeld(); }

  // The driver's ID for this mutex, or 0 if it is not ordered. Unlike the
  // address of the mutex, this is the same when replaying as when recording.
  int ordered_lock_id() const { return ordered_lock_id_; }

 private:
  absl::Mutex mutex_;
  const int ordered_lock_id_;

  // Whether the current holder of `mutex_` also holds the driver's ordered
  // lock. Only accessed while `mutex_` is held.
  bool holds_ordered_lock_ = false;
};

// absl::MutexLock for an OrderedMutex.
class ABSL_SCOPED_LOCKABLE OrderedMutexLock {
 public:
  explicit OrderedMutexLock(OrderedMutex* mutex)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(mutex)
      : mutex_(mutex) {
    mutex_->Lock();
  }
  OrderedMutexLock(const OrderedMutexLock&) = delete;
  OrderedMutexLock& operator=(const OrderedMutexLock&) = delete;
  ~OrderedMutexLock() ABSL_UNLOCK_FUNCTION() { mutex_->Unlock(); }

 private:
  OrderedMutex* const mutex_;
};

// absl::ReleasableMutexLock for an OrderedMutex.
class ABSL_SCOPED_LOCKABLE ReleasableOrderedMutexLock {
 public:
  explicit ReleasableOrderedMutexLock(OrderedMutex* mutex)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(mutex)
      : mutex_(mutex) {
    mutex_->Lock();
  }
  ReleasableOrderedMutexLock(const ReleasableOrderedMutexLock&) = delete;
  ReleasableOrderedMutexLock& operator=(const ReleasableOrderedMutexLock&) =
      delete;
  ~ReleasableOrderedMutexLock() ABSL_UNLOCK_FUNCTION() {
    if (mutex_) {
      mutex_->Unlock();
    }
  }

  void Release() ABSL_UNLOCK_FUNCTION() {
    mutex_->Unlock();
    mutex_ = nullptr;
  }

 private:
  OrderedMutex* mutex_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_ORDERED_MUTEX_H_
