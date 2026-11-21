// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/ref_counted.h"

#include "third_party/abseil-cpp/absl/base/macros.h"
#include "util/record_replay.h"

namespace ipcz::internal {

RefCountedBase::RefCountedBase() = default;

RefCountedBase::~RefCountedBase() = default;

void RefCountedBase::AcquireImpl() {
  ref_count_.fetch_add(1, std::memory_order_relaxed);
}

// [RecordReplay] ipcz objects (routers, links, the objects a parcel carries)
// are held by the thread which receives from a transport and by the threads
// which use portals, and whichever drops the last reference destroys the
// object, which closes routes, takes ordered locks and closes driver objects.
// Which thread that is has to be the same when replaying, so decrements are
// ordered among threads with one lock. Increments are left alone, for the
// reason given at recordreplay::AutoOrderedRelease, and the destruction itself
// happens outside the lock. Nothing is ordered in regions which cannot add to
// the recording.
bool RefCountedBase::ReleaseImpl() {
  // SUBTLE: Technically the load does not need to be an acquire unless we're
  // releasing the last reference and need to delete `this`, but it's not clear
  // whether std::memory_order_acq_rel here will produce more or less efficient
  // code compared to a plain std::memory_order_release followed by an acquire
  // fence in the conditional block below.
  replay::AutoOrderedRelease ordered;
  int last_count = ref_count_.fetch_sub(1, std::memory_order_acq_rel);
  ABSL_ASSERT(last_count > 0);
  return last_count == 1;
}

}  // namespace ipcz::internal
