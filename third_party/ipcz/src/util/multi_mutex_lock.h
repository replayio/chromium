// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_MULTI_MUTEX_LOCK_H_
#define IPCZ_SRC_UTIL_MULTI_MUTEX_LOCK_H_

#include <algorithm>
#include <array>
#include <cstddef>

#include "base/record_replay.h"
#include "util/ordered_mutex.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

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
    // [Replay] Sorting by raw address picks an acquisition order that no replay
    // can reproduce: heap addresses are the replay's own.  Two threads taking
    // the same pair of router mutexes can therefore interleave differently in
    // record and replay.  Sort by the registered pointer id instead -- the
    // fork's standard comparator, and what M108 did for this machinery's
    // predecessor (GetPortId, mojo/core/ports/port_locker.cc).  DSRC-275.
    //
    std::sort(mutexes_.begin(), mutexes_.end(),
              recordreplay::CompareByPointerId{});
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
