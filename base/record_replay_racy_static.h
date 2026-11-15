// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_RECORD_REPLAY_RACY_STATIC_H_
#define BASE_RECORD_REPLAY_RACY_STATIC_H_

namespace recordreplay {

// While one of these is alive on a thread, a base::Lock it creates without a
// name is left unordered. For an object that whichever thread gets there first
// makes (a function-local static shared by threads): that isn't the same
// thread on a replay, so the lock's creation can't be in either thread's part
// of the recording. Everything else the constructor does is recorded as usual.
class AutoUnorderedLockCreation {
 public:
  AutoUnorderedLockCreation();
  ~AutoUnorderedLockCreation();
  AutoUnorderedLockCreation(const AutoUnorderedLockCreation&) = delete;
  AutoUnorderedLockCreation& operator=(const AutoUnorderedLockCreation&) =
      delete;
};

bool InUnorderedLockCreation();

}  // namespace recordreplay

#endif  // BASE_RECORD_REPLAY_RACY_STATIC_H_
