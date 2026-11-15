// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_MOJO_RECORD_REPLAY_THREAD_REMOTE_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_MOJO_RECORD_REPLAY_THREAD_REMOTE_H_

#include "base/memory/raw_ptr.h"
#include "base/record_replay.h"
#include "base/task/current_thread.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace blink {

// A remote a worker thread keeps for itself in thread-specific storage is
// destroyed as the thread exits, by the destructor of that storage. That is
// after the thread has left its recording, so closing the pipe there is
// ordered with nothing: it takes the pipe's traps away at one moment when
// recording and another when replaying, and the thread receiving for the
// process then finds different traps on a message's arrival.
//
// Call this once the remote is bound. When recording or replaying, the remote
// is reset as the thread's task environment is torn down, which is still part
// of its recording; nothing is left for the storage's destructor to close.
template <typename Interface>
void RecordReplayResetBeforeThreadEnds(mojo::Remote<Interface>* remote) {
  if (!recordreplay::IsRecordingOrReplaying() || recordreplay::IsMainThread() ||
      !base::CurrentThread::IsSet()) {
    return;
  }
  class Observer : public base::CurrentThread::DestructionObserver {
   public:
    explicit Observer(mojo::Remote<Interface>* remote) : remote_(remote) {}
    void WillDestroyCurrentMessageLoop() override {
      remote_->reset();
      base::CurrentThread::Get()->RemoveDestructionObserver(this);
      delete this;
    }

   private:
    ~Observer() override = default;
    const raw_ptr<mojo::Remote<Interface>> remote_;
  };
  base::CurrentThread::Get()->AddDestructionObserver(new Observer(remote));
}

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_MOJO_RECORD_REPLAY_THREAD_REMOTE_H_
