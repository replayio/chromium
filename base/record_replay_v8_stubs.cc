// Copyright (c) 2021 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Weak fallbacks for the V8 driver entry points //base calls.
//
// WHY THIS FILE EXISTS. The fork instruments //base widely -- 37 objects in
// libbase.a reference `recordreplay::` -- and `recordreplay::` is implemented in
// base/record_replay.cc, which on POSIX declares V8's 68 `V8*` exports as plain
// `extern "C"`. V8 sits ABOVE base, so any binary that links base but not V8
// has 68 unresolved symbols, and every Chromium executable is linked with
// `-Wl,-z,defs`, which forbids them. Host tools are exactly that case:
// root_store_tool pulls base objects that need `recordreplay::`, which pulls
// record_replay.o, which needs V8.
//
// WHY WEAK IS THE RIGHT ANSWER HERE. On Windows the fork already tolerates an
// absent driver: it binds the same table through GetProcAddress and each entry
// returns the DefaultValue the table carries when the lookup fails. These
// definitions give POSIX the same behaviour with the same DefaultValues, so
// nothing about the fork's intent changes -- only the mechanism by which an
// absent driver is tolerated.
//
// WHY A SEPARATE TRANSLATION UNIT. If these definitions sat in
// base/record_replay.cc beside the calls, the compiler could bind those calls
// directly to the local weak definitions -- they are `-fvisibility=hidden` and
// so not interposable -- and record/replay would be silently dead in Chrome.
// Here the callers' references stay undefined in their own TU and are resolved
// at link time, where a strong definition always wins over a weak one. In the
// chrome link V8's api.o defines all 68 strongly, so these are discarded; in a
// host-tool link nothing else defines them, so these are used.
//
// This is verifiable, and must be verified rather than assumed: disassemble
// V8IsRecordingOrReplaying in the linked chrome binary and check it loads V8's
// function pointer rather than returning an immediate.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <string>

#include "build/build_config.h"

#if !BUILDFLAG(IS_WIN)

#include "base/record_replay_v8_api.h"
// RecordReplayWarningField; self-contained, no //base dependency.
#include "v8/include/replayio-macros.h"

#define DefineWeakStub(Name, Formals, Args, ReturnType, DefaultValue) \
  extern "C" __attribute__((weak)) ReturnType Name Formals {          \
    return DefaultValue;                                              \
  }
ForEachV8API(DefineWeakStub)
#undef DefineWeakStub

#define DefineWeakStubVoid(Name, Formals, Args) \
  extern "C" __attribute__((weak)) void Name Formals {}
ForEachV8APIVoid(DefineWeakStubVoid)
#undef DefineWeakStubVoid

#endif  // !BUILDFLAG(IS_WIN)
