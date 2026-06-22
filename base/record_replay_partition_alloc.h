// Copyright (c) 2026 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Minimal partition_alloc-safe shim that exposes only the small subset of
// the recordreplay API needed by partition_alloc translation units.
//
// partition_alloc is built as a standalone, portable library with stricter
// warning/plugin configuration than the rest of //base (e.g. -Wgcc-compat is
// fatal, the chromium-rawref plugin is enforced, transitive STL pulls are
// limited). Including the full base/record_replay.h (which transitively pulls
// base/synchronization/lock.h -> base/containers/span.h, with clang-only
// `enable_if` attributes) breaks the partition_alloc build.
//
// Use this header from partition_alloc *.cc files instead of
// base/record_replay.h. Only forward declarations of the recordreplay
// entry points actually used from partition_alloc are exposed here; the
// implementations still live in the normal recordreplay translation units.

#ifndef BASE_RECORD_REPLAY_PARTITION_ALLOC_H_
#define BASE_RECORD_REPLAY_PARTITION_ALLOC_H_

namespace recordreplay {

bool IsRecordingOrReplaying(const char* feature = nullptr,
                            const char* subfeature = nullptr);

void Assert(const char* format, ...);

void BeginDisallowEventsWithLabel(const char* label);
void EndDisallowEvents();

struct AutoDisallowEvents {
  AutoDisallowEvents(const char* label) { BeginDisallowEventsWithLabel(label); }
  ~AutoDisallowEvents() { EndDisallowEvents(); }
};

}  // namespace recordreplay

#endif  // BASE_RECORD_REPLAY_PARTITION_ALLOC_H_
