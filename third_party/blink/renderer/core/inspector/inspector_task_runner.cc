// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/inspector/inspector_task_runner.h"

#include "third_party/blink/renderer/platform/bindings/thread_debugger.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_copier_base.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"

#include "base/record_replay.h"

namespace blink {

InspectorTaskRunner::InspectorTaskRunner(
    scoped_refptr<base::SingleThreadTaskRunner> isolate_task_runner)
    : lock_("InspectorTaskRunner.lock_"), isolate_task_runner_(isolate_task_runner) {}

InspectorTaskRunner::~InspectorTaskRunner() = default;

void InspectorTaskRunner::InitIsolate(v8::Isolate* isolate) {
  base::AutoLock locker(lock_);
  isolate_ = isolate;
}

void InspectorTaskRunner::Dispose() {
  // Declared before the lock, so the tasks are destroyed after it's released.
  Deque<Task> tasks;
  recordreplay::AutoLockMaybeEventsDisallowed locker(lock_);
  disposed_ = true;
  isolate_ = nullptr;
  isolate_task_runner_ = nullptr;
  // Queued tasks never run once this is disposed. When recording/replaying,
  // destroy them here, at a point ordered with AppendTask(), instead of with
  // this runner: which thread drops its last reference isn't recorded, and a
  // task's destruction can close mojo handles, which takes ordered locks.
  if (recordreplay::IsRecordingOrReplaying() &&
      !recordreplay::AreEventsDisallowed()) {
    tasks.Swap(interrupting_task_queue_);
  }
}

scoped_refptr<base::SingleThreadTaskRunner>
InspectorTaskRunner::isolate_task_runner() {
  // Dispose() clears the runner from another thread, and a task taken before
  // it can read the runner after it. When recording/replaying, read it under
  // the lock, which orders the read with Dispose().
  if (recordreplay::IsRecordingOrReplaying()) {
    base::AutoLock locker(lock_);
    return isolate_task_runner_;
  }
  return isolate_task_runner_;
}

bool InspectorTaskRunner::AppendTask(Task task) {
  base::AutoLock locker(lock_);
  if (disposed_)
    return false;
  interrupting_task_queue_.push_back(std::move(task));
  PostCrossThreadTask(
      *isolate_task_runner_, FROM_HERE,
      CrossThreadBindOnce(
          &InspectorTaskRunner::PerformSingleInterruptingTaskDontWait,
          WrapRefCounted(this)));
  if (isolate_) {
    AddRef();
    isolate_->RequestInterrupt(&V8InterruptCallback, this);
  }
  return true;
}

bool InspectorTaskRunner::AppendTaskDontInterrupt(Task task) {
  base::AutoLock locker(lock_);
  if (disposed_)
    return false;
  PostCrossThreadTask(*isolate_task_runner_, FROM_HERE, std::move(task));
  return true;
}

InspectorTaskRunner::Task InspectorTaskRunner::TakeNextInterruptingTask() {
  base::AutoLock locker(lock_);

  if (disposed_ || interrupting_task_queue_.empty())
    return Task();

  return interrupting_task_queue_.TakeFirst();
}

void InspectorTaskRunner::PerformSingleInterruptingTaskDontWait() {
  Task task = TakeNextInterruptingTask();
  if (task) {
    DCHECK(isolate_task_runner_->BelongsToCurrentThread());
    std::move(task).Run();
  }
}

void InspectorTaskRunner::V8InterruptCallback(v8::Isolate*, void* data) {
  InspectorTaskRunner* runner = static_cast<InspectorTaskRunner*>(data);
  Task task = runner->TakeNextInterruptingTask();
  runner->Release();
  if (!task) {
    return;
  }
  std::move(task).Run();
}

}  // namespace blink
