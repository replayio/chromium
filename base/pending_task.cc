// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/pending_task.h"

#include "base/dcheck_is_on.h"
#include "base/debug/alias.h"
#include "base/record_replay.h"

namespace base {

#if DCHECK_IS_ON()
namespace {

// Returns `str`, or an empty string if `str` is null.
const char* EmptyIfNull(const char* str) {
  if (str) {
    return str;
  }
  return "";
}

}  // namespace
#endif

TaskMetadata::TaskMetadata() = default;

TaskMetadata::TaskMetadata(const Location& posted_from,
                           TimeTicks queue_time,
                           TimeTicks delayed_run_time,
                           TimeDelta leeway,
                           subtle::DelayPolicy delay_policy)
    : posted_from(posted_from),
      queue_time(queue_time),
      delayed_run_time(delayed_run_time),
      leeway(leeway),
      delay_policy(delay_policy) {
  // [RecordReplay] RUN-1219: the task id is minted here rather than in
  // PendingTask, because from M121 upstream splits PendingTask into this
  // TaskMetadata base plus the closure, and this is the one constructor every
  // "a task is created at a posting site" path goes through.  Minting in
  // PendingTask(const TaskMetadata&, OnceClosure) instead would make copying a
  // TaskMetadata change its identity, which its defaulted copy constructor
  // does not.
  if (!recordreplay::AreEventsUnavailable() &&
      !recordreplay::AreEventsPassedThrough()) {
    record_replay_id = recordreplay::NewIdAnyThread("PendingTask");
  }
}

TaskMetadata::TaskMetadata(TaskMetadata&& other) = default;
TaskMetadata::TaskMetadata(const TaskMetadata& other) = default;

TaskMetadata::~TaskMetadata() = default;

TaskMetadata& TaskMetadata::operator=(TaskMetadata&& other) = default;
TaskMetadata& TaskMetadata::operator=(const TaskMetadata& other) = default;

PendingTask::PendingTask() = default;

PendingTask::PendingTask(const Location& posted_from,
                         OnceClosure task,
                         TimeTicks queue_time,
                         TimeTicks delayed_run_time,
                         TimeDelta leeway,
                         subtle::DelayPolicy delay_policy)
    : TaskMetadata(posted_from,
                   queue_time,
                   delayed_run_time,
                   leeway,
                   delay_policy),
      task(std::move(task)) {}

PendingTask::PendingTask(const TaskMetadata& metadata, OnceClosure task)
    : TaskMetadata(metadata), task(std::move(task)) {}

PendingTask::PendingTask(PendingTask&& other) = default;

PendingTask::~PendingTask() {
#if DCHECK_IS_ON()
  // Instrumentation to investigate crbug.com/1494307 (only required in
  // DCHECK-enabled builds since this is a DCHECK failure).
  // TODO(crbug.com/1494307): Remove after March 2024.
  DEBUG_ALIAS_FOR_CSTR(posted_from_function,
                       EmptyIfNull(posted_from.function_name()), 256);
  DEBUG_ALIAS_FOR_CSTR(posted_from_file, EmptyIfNull(posted_from.file_name()),
                       256);
#endif
  task.Reset();
}

PendingTask& PendingTask::operator=(PendingTask&& other) = default;

TimeTicks TaskMetadata::GetDesiredExecutionTime() const {
  if (!delayed_run_time.is_null())
    return delayed_run_time;
  return queue_time;
}

TimeTicks TaskMetadata::earliest_delayed_run_time() const {
  DCHECK(!delayed_run_time.is_null());
  if (delay_policy == subtle::DelayPolicy::kFlexiblePreferEarly)
    return delayed_run_time - leeway;
  return delayed_run_time;
}

TimeTicks TaskMetadata::latest_delayed_run_time() const {
  DCHECK(!delayed_run_time.is_null());
  if (delay_policy == subtle::DelayPolicy::kFlexibleNoSooner)
    return delayed_run_time + leeway;
  return delayed_run_time;
}

}  // namespace base
