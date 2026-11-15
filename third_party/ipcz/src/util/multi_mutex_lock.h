// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_MULTI_MUTEX_LOCK_H_
#define IPCZ_SRC_UTIL_MULTI_MUTEX_LOCK_H_

#include <algorithm>
#include <array>
#include <cstddef>

#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/ordered_mutex.h"

namespace ipcz {

// MultiMutexLock is a scoped mutex locker capable of locking between two and
// four mutexes simultaneously. Locks are always acquired in a globally
// consistent order  based on the address of each Mutex.
template <size_t N>
class ABSL_SCOPED_LOCKABLE MultiMutexLock {
 public:
  MultiMutexLock(OrderedMutex* a, OrderedMutex* b)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(*a, *b)
      : mutexes_({a, b}) {
    SortAndLock();
  }

  MultiMutexLock(OrderedMutex* a, OrderedMutex* b, OrderedMutex* c)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(*a, *b, *c)
      : mutexes_({a, b, c}) {
    SortAndLock();
  }

  MultiMutexLock(OrderedMutex* a, OrderedMutex* b, OrderedMutex* c, OrderedMutex* d)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(*a, *b, *c, *d)
      : mutexes_({a, b, c, d}) {
    SortAndLock();
  }

  ~MultiMutexLock() ABSL_UNLOCK_FUNCTION() {
    for (OrderedMutex* mutex : mutexes_) {
      mutex->Unlock();
    }
  }

 private:
  void SortAndLock() ABSL_NO_THREAD_SAFETY_ANALYSIS {
    // When recording/replaying the order in which the mutexes are acquired
    // needs to be consistent, and their addresses are not. Sort by the IDs of
    // their ordered locks instead, like the legacy core's PortLocker sorted
    // ports by pointer ID.
    std::sort(mutexes_.begin(), mutexes_.end(),
              [](OrderedMutex* a, OrderedMutex* b) {
                if (a->ordered_lock_id() != b->ordered_lock_id()) {
                  return a->ordered_lock_id() < b->ordered_lock_id();
                }
                return a < b;
              });
    for (OrderedMutex* mutex : mutexes_) {
      mutex->Lock();
    }
  }

  std::array<OrderedMutex*, N> mutexes_;
};

// Helpful deduction guides so instantiations can omit a template argument.
MultiMutexLock(OrderedMutex*, OrderedMutex*)->MultiMutexLock<2>;
MultiMutexLock(OrderedMutex*, OrderedMutex*, OrderedMutex*)->MultiMutexLock<3>;
MultiMutexLock(OrderedMutex*, OrderedMutex*, OrderedMutex*, OrderedMutex*)
    ->MultiMutexLock<4>;

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_MULTI_MUTEX_LOCK_H_
