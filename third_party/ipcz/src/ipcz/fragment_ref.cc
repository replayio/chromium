// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/fragment_ref.h"

#include "base/record_replay.h"

#include <algorithm>
#include <utility>

#include "ipcz/fragment.h"
#include "ipcz/node_link_memory.h"
#include "ipcz/ref_counted_fragment.h"
#include "util/ref_counted.h"

namespace ipcz::internal {

GenericFragmentRef::GenericFragmentRef() = default;

GenericFragmentRef::GenericFragmentRef(Ref<NodeLinkMemory> memory,
                                       const Fragment& fragment)
    : memory_(std::move(memory)), fragment_(fragment) {}

GenericFragmentRef::~GenericFragmentRef() {
  reset();
}

void GenericFragmentRef::reset() {
  Ref<NodeLinkMemory> memory = std::move(memory_);
  if (fragment_.is_null()) {
    return;
  }

  Fragment fragment;
  std::swap(fragment, fragment_);
  if (!fragment.is_addressable()) {
    return;
  }

  auto* ref_counted = static_cast<RefCountedFragment*>(fragment.address());
  // [Replay] The reference count lives in the shared fragment and the PEER
  // also holds references, so a replay decrements a count it cannot see and
  // reaches a different verdict about who holds the last one -- which decides
  // whether FreeFragment runs at all.  Record the verdict; the decrement
  // itself is unconditional here, so the event is too.  DSRC-245.
  const bool was_last_ref = !!recordreplay::RecordReplayValue(
      "GenericFragmentRef::ReleaseRef", ref_counted->ReleaseRef() <= 1);
  if (!was_last_ref || !memory) {
    return;
  }

  memory->FreeFragment(fragment);
}

Fragment GenericFragmentRef::release() {
  Fragment fragment;
  std::swap(fragment_, fragment);
  memory_.reset();
  return fragment;
}

}  // namespace ipcz::internal
