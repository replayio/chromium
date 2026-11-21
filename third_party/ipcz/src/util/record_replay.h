// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_RECORD_REPLAY_H_
#define IPCZ_SRC_UTIL_RECORD_REPLAY_H_

#include <cstddef>
#include <cstdint>

#include "base/record_replay.h"

// ipcz' view of the record/replay driver API declared in base/record_replay.h.
//
// Two nodes share memory (link states, fragment reference counts, ID counters,
// block allocators and parcel data), and only one of them is recorded. When
// that node replays, the other node is not there and the memory holds only
// what the replaying node wrote itself. Everything a node reads from memory
// its peer can write is therefore an input: it is recorded when recording and
// taken from the recording when replaying.
namespace ipcz::replay {

// Whether values and ordered locks can be added to the recording from the
// current thread right now. False outside of a recording, and in regions which
// run at non-deterministic points (events disallowed or passed through).
inline bool IsRecordable() {
  return ::recordreplay::IsRecordingOrReplaying() &&
         !::recordreplay::AreEventsDisallowed() &&
         !::recordreplay::AreEventsPassedThrough() &&
         !::recordreplay::HasDivergedFromRecording();
}

// `value` when recording, what it was when recording when replaying.
inline uintptr_t Value(const char* why, uintptr_t value) {
  return IsRecordable() ? ::recordreplay::RecordReplayValue(why, value) : value;
}

// Leaves `buffer` untouched when recording, fills it with its recorded
// contents when replaying.
inline void Bytes(const char* why, void* buffer, size_t size) {
  if (size && IsRecordable()) {
    ::recordreplay::RecordReplayBytes(why, buffer, size);
  }
}

// Creates a driver ordered lock, or returns 0 when the current thread cannot
// add anything to the recording.
inline int CreateOrderedLock(const char* name) {
  return IsRecordable() ? ::recordreplay::CreateOrderedLock(name) : 0;
}

// Orders one reference count decrement among threads. See
// recordreplay::AutoOrderedRelease: one lock serves the counts of ipcz'
// objects and of the Mojo driver's objects.
using AutoOrderedRelease = ::recordreplay::AutoOrderedRelease;

}  // namespace ipcz::replay

#endif  // IPCZ_SRC_UTIL_RECORD_REPLAY_H_
