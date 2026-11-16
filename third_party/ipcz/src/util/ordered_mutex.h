// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_ORDERED_MUTEX_H_
#define IPCZ_SRC_UTIL_ORDERED_MUTEX_H_

#include "base/synchronization/lock.h"
#include "third_party/abseil-cpp/absl/base/thread_annotations.h"

namespace ipcz {

// [Replay] A mutex whose acquisition order is reproduced in a replay.
//
// ipcz guards all of its route state with absl::Mutex, which this fork cannot
// order: it is a hand-rolled futex with no pthread_mutex_t for the driver to
// register, and abseil says so itself --
// "currently absl mutexes don't support ordering"
// (absl/synchronization/mutex.cc).  The consequence is not theoretical: route
// closure is delivered on one thread while another serializes the router, so
// `Router::is_peer_closed_` was observed as 1 while recording and 0 while
// replaying for the same sublink, which sends Router::Flush down a different
// branch ~700 events later.  See DSRC-276.
//
// base::Lock IS orderable when constructed with a name -- the named ctor hands
// the driver its native pthread mutex -- which is how M108 ordered every lock
// in the mojo core this code replaced (Port.lock_, Node.ports_lock_, ...).
// Ordering absl::Mutex itself is NOT an option: v8::base::Mutex is an
// absl::Mutex, so that would order all of V8, including paths that run with
// events disallowed.
//
// The abseil lock annotations are kept so ABSL_GUARDED_BY on the guarded
// members keeps working unchanged.
class ABSL_LOCKABLE OrderedMutex {
 public:
  explicit OrderedMutex(const char* ordered_name) : lock_(ordered_name) {}

  OrderedMutex(const OrderedMutex&) = delete;
  OrderedMutex& operator=(const OrderedMutex&) = delete;

  void Lock() ABSL_EXCLUSIVE_LOCK_FUNCTION() { lock_.Acquire(); }
  void Unlock() ABSL_UNLOCK_FUNCTION() { lock_.Release(); }
  void AssertHeld() const ABSL_ASSERT_EXCLUSIVE_LOCK() {
    lock_.AssertAcquired();
  }

 private:
  mutable base::Lock lock_;
};

// Drop-in replacement for absl::MutexLock.
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

// Drop-in replacement for absl::ReleasableMutexLock.
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
    if (mutex_) {
      mutex_->Unlock();
      mutex_ = nullptr;
    }
  }

 private:
  OrderedMutex* mutex_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_ORDERED_MUTEX_H_
