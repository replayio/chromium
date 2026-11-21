// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_RECORD_REPLAY_ISOLATED_DRAW_ASSERTS_H_
#define BASE_RECORD_REPLAY_ISOLATED_DRAW_ASSERTS_H_

#include <atomic>

#include "base/base_export.h"
#include "base/compiler_specific.h"

// Asserts that Replay's isolated in-process draws don't touch what recorded
// threads share (locks, counters, histogram samples, IDs), enabled by
// RECORD_REPLAY_ISOLATED_DRAW_ASSERTS: a failed one prints what was touched
// and its caller, then crashes. With them come notes that only print. They are
// hooked into hot base code (locks, sequence numbers), so when disabled each
// costs a load and a branch.
// Kept apart from base/record_replay.cc, which links against V8: binaries
// without V8 use these hooks too.

namespace recordreplay {

// 0 until the environment is read, then 1 when disabled and 2 when enabled.
BASE_EXPORT extern std::atomic<int> gIsolatedDrawAssertsState;
BASE_EXPORT bool ReadIsolatedDrawAsserts();

inline bool IsolatedDrawAssertsEnabled() {
  int state = gIsolatedDrawAssertsState.load(std::memory_order_relaxed);
  if (LIKELY(state == 1)) {
    return false;
  }
  return state == 2 || ReadIsolatedDrawAsserts();
}

// Isolated draws on this thread, as SkRecordReplayBeginIsolatedDraw counts
// them in Skia, which base can't call. Tracked only when enabled.
BASE_EXPORT void SetInIsolatedDrawForAsserts(bool in);
BASE_EXPORT bool InIsolatedDrawForAsserts();

BASE_EXPORT NOINLINE void IsolatedDrawAssertSlow(const char* what,
                                                 const void* address);
BASE_EXPORT NOINLINE void IsolatedDrawAssertNameSlow(const char* what,
                                                     const char* name);
BASE_EXPORT NOINLINE void IsolatedDrawAssertAlwaysSlow(const char* what,
                                                       const void* address);
BASE_EXPORT NOINLINE void IsolatedDrawNoteSlow(const char* what,
                                               const void* address);

// Fails when this thread is in an isolated draw.
inline void IsolatedDrawAssert(const char* what, const void* address) {
  if (UNLIKELY(IsolatedDrawAssertsEnabled())) {
    IsolatedDrawAssertSlow(what, address);
  }
}

inline void IsolatedDrawAssertName(const char* what, const char* name) {
  if (UNLIKELY(IsolatedDrawAssertsEnabled())) {
    IsolatedDrawAssertNameSlow(what, name);
  }
}

// Fails wherever it happens.
inline void IsolatedDrawAssertAlways(const char* what, const void* address) {
  if (UNLIKELY(IsolatedDrawAssertsEnabled())) {
    IsolatedDrawAssertAlwaysSlow(what, address);
  }
}

// Prints |what|, which is worth knowing about but not a failure.
inline void IsolatedDrawNote(const char* what, const void* address) {
  if (UNLIKELY(IsolatedDrawAssertsEnabled())) {
    IsolatedDrawNoteSlow(what, address);
  }
}

}  // namespace recordreplay

#endif  // BASE_RECORD_REPLAY_ISOLATED_DRAW_ASSERTS_H_
