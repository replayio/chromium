// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/thread_pool/task.h"

#include <utility>

#include "base/atomic_sequence_num.h"
#include "base/record_replay.h"

namespace base {
namespace internal {

namespace {

AtomicSequenceNumber g_sequence_nums_for_tracing;

}  // namespace

Task::Task(const Location& posted_from,
           OnceClosure task,
           TimeTicks queue_time,
           TimeDelta delay,
           TimeDelta leeway)
    : Task(posted_from,
           std::move(task),
           queue_time,
           delay.is_zero() ? TimeTicks() : queue_time + delay,
           leeway) {}

Task::Task(const Location& posted_from,
           OnceClosure task,
           TimeTicks queue_time,
           TimeTicks delayed_run_time,
           TimeDelta leeway,
           subtle::DelayPolicy delay_policy)
    : PendingTask(posted_from,
                  std::move(task),
                  queue_time,
                  delayed_run_time,
                  leeway,
                  delay_policy) {
  // ThreadPoolImpl doesn't use |sequence_num| but tracing (toplevel.flow)
  // relies on it being unique. While this subtle dependency is a bit
  // overreaching, ThreadPoolImpl is the only task system that doesn't use
  // |sequence_num| and the dependent code rarely changes so this isn't worth a
  // big change and faking it here isn't too bad for now (posting tasks is full
  // of atomic ops already).
  this->sequence_num = g_sequence_nums_for_tracing.GetNext();

  // DelayedTaskManager and Sequence break ties between equal delayed run times
  // (common for WebRTC task queues, which snap to metronome ticks) with
  // |sequence_num|, so the replay needs the numbers the recording handed out
  // across threads.
  if (recordreplay::IsRecordingOrReplaying("values", "ThreadPoolTaskSequenceNum")) {
    this->sequence_num = static_cast<int>(recordreplay::RecordReplayValue(
        "ThreadPoolTaskSequenceNum", static_cast<uintptr_t>(this->sequence_num)));
  }
}

// This should be "= default but MSVC has trouble with "noexcept = default" in
// this case.
Task::Task(Task&& other) noexcept : PendingTask(std::move(other)) {}

Task& Task::operator=(Task&& other) = default;

}  // namespace internal
}  // namespace base
