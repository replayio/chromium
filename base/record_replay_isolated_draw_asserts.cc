// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/record_replay_isolated_draw_asserts.h"

#include <stdarg.h>
#include <stdlib.h>

#include "base/immediate_crash.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include <dlfcn.h>
#endif

namespace recordreplay {

std::atomic<int> gIsolatedDrawAssertsState;

bool ReadIsolatedDrawAsserts() {
  bool enabled = !!getenv("RECORD_REPLAY_ISOLATED_DRAW_ASSERTS");
  gIsolatedDrawAssertsState.store(enabled ? 2 : 1, std::memory_order_relaxed);
  return enabled;
}

static thread_local int gIsolatedDrawDepth;

void SetInIsolatedDrawForAsserts(bool in) {
  gIsolatedDrawDepth += in ? 1 : -1;
}

bool InIsolatedDrawForAsserts() {
  return gIsolatedDrawDepth > 0;
}

// Prints with the driver's RecordReplayPrint, which binaries without V8 can't
// link.
static void AssertPrint(const char* format, ...) {
#if BUILDFLAG(IS_POSIX)
  static void (*print)(const char*, va_list) =
      reinterpret_cast<void (*)(const char*, va_list)>(
          dlsym(RTLD_DEFAULT, "RecordReplayPrint"));
  if (!print) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  print(format, ap);
  va_end(ap);
#endif
}

void IsolatedDrawAssertSlow(const char* what, const void* address) {
  if (InIsolatedDrawForAsserts()) {
    IsolatedDrawAssertAlwaysSlow(what, address);
  }
}

void IsolatedDrawAssertNameSlow(const char* what, const char* name) {
  if (InIsolatedDrawForAsserts()) {
    AssertPrint("IsolatedDrawAssert failed: %s %s", what, name);
    IMMEDIATE_CRASH();
  }
}

void IsolatedDrawAssertAlwaysSlow(const char* what, const void* address) {
  AssertPrint("IsolatedDrawAssert failed: %s caller=%p", what, address);
  IMMEDIATE_CRASH();
}

// Notes can be many, e.g. one for each paint without a sent frame, so only
// the first ones print.
void IsolatedDrawNoteSlow(const char* what, const void* address) {
  static std::atomic<int> count;
  if (count++ < 2000) {
    AssertPrint("IsolatedDrawNote %s caller=%p", what, address);
  }
}

}  // namespace recordreplay
