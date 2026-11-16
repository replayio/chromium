// Copyright (c) 2021 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The record/replay calls partition_alloc needs, bound straight to the driver.
//
// partition_alloc is the lowest layer of the tree and is linked into binaries
// that contain no V8 -- host tools such as root_store_tool. It therefore cannot
// use base/record_replay.h: that would make partition_alloc's archives
// reference `recordreplay::`, whose implementation in //base in turn references
// V8's `V8*` driver exports, and every Chromium executable links with
// `-Wl,-z,defs`, so such a link fails outright.
//
// This is the pattern base/record_replay_inline.h already documents as "for use
// in situations when the recordreplay:: namespace isn't available due to build
// dependency ordering": look the driver's own exports up at run time and no-op
// when the driver is absent. It creates no link dependency in either direction.
//
// IsRecordingOrReplaying() here is EXACTLY equivalent to
// recordreplay::IsRecordingOrReplaying() with no feature argument. Measured in
// v8/src/api/api.cc:
//     bool recordreplay::IsRecordingOrReplaying(const char* feature,
//                                               const char* subfeature) {
//       return internal::gRecordReplayIsRecordingOrReplaying &&
//              (!feature || FeatureEnabled(feature, subfeature));
//     }
// With `feature == nullptr` that is precisely "the driver's
// RecordReplayIsRecordingOrReplaying symbol was loaded", which is what the
// lookup below tests.
//
// The lookups are deliberately NOT cached in a function-local static: a static
// initializer needs a guard variable, and the replay linker binds
// __cxa_guard_acquire to a bare compare-exchange, so static initializers are
// not serialized under replay. Both call sites are cold -- allocator hook
// installation and one address-space randomization during partition_alloc
// startup -- so an uncached lookup costs nothing that matters.

#ifndef PARTITION_ALLOC_RECORD_REPLAY_SHIM_H_
#define PARTITION_ALLOC_RECORD_REPLAY_SHIM_H_

#include "partition_alloc/build_config.h"
#include "partition_alloc/buildflags.h"
#include "partition_alloc/partition_alloc_base/memory/stack_allocated.h"

#if PA_BUILDFLAG(IS_WIN)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace partition_alloc::record_replay {

// The driver's export, or nullptr when no driver is loaded.
inline void* DriverSymbol(const char* name) {
#if PA_BUILDFLAG(IS_WIN)
  HMODULE module = ::GetModuleHandleA("windows-recordreplay.dll");
  return module ? reinterpret_cast<void*>(::GetProcAddress(module, name))
                : nullptr;
#else
  return ::dlsym(RTLD_DEFAULT, name);
#endif
}

inline bool IsRecordingOrReplaying() {
  return DriverSymbol("RecordReplayIsRecordingOrReplaying") != nullptr;
}

// Mirrors recordreplay::AutoDisallowEvents. Does nothing without a driver, and
// only calls End when Begin was actually reached, so the two stay balanced.
class AutoDisallowEvents {
  PA_STACK_ALLOCATED();

 public:
  explicit AutoDisallowEvents(const char* label) {
    void* begin = DriverSymbol("RecordReplayBeginDisallowEventsWithLabel");
    if (!begin) {
      return;
    }
    reinterpret_cast<void (*)(const char*)>(begin)(label);
    end_ = DriverSymbol("RecordReplayEndDisallowEvents");
  }

  AutoDisallowEvents(const AutoDisallowEvents&) = delete;
  AutoDisallowEvents& operator=(const AutoDisallowEvents&) = delete;

  ~AutoDisallowEvents() {
    if (end_) {
      reinterpret_cast<void (*)()>(end_)();
    }
  }

 private:
  void* end_ = nullptr;
};

}  // namespace partition_alloc::record_replay

#endif  // PARTITION_ALLOC_RECORD_REPLAY_SHIM_H_
