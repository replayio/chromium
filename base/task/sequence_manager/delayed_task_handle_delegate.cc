// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/sequence_manager/delayed_task_handle_delegate.h"

#include "base/record_replay.h"
#include "base/task/sequence_manager/task_queue_impl.h"

namespace base::sequence_manager::internal {

DelayedTaskHandleDelegate::DelayedTaskHandleDelegate(TaskQueueImpl* outer)
    : outer_(outer) {}

DelayedTaskHandleDelegate::~DelayedTaskHandleDelegate() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!IsValid());
}

WeakPtr<DelayedTaskHandleDelegate> DelayedTaskHandleDelegate::AsWeakPtr() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return weak_ptr_factory_.GetWeakPtr();
}

bool DelayedTaskHandleDelegate::IsValid() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return weak_ptr_factory_.HasWeakPtrs();
}

void DelayedTaskHandleDelegate::CancelTask() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!IsValid()) {
    return;
  }

  // Use InvalidateWeakPtrsAndDoom() to avoid allocating a new
  // WeakReference::Flag, as no new WeakPtrs will ever be issued from this
  // delegate.
  weak_ptr_factory_.InvalidateWeakPtrsAndDoom();

  // [Replay] A GC finalizer cancels delayed tasks: ~Document destroys its
  // element_data_cache_clear_timer_, ~TimerBase calls Stop(), and that lands
  // here while cppgc is sweeping inside one of the fork's GC disallow scopes.
  // Removing the task from the heap walks the whole scheduling stack --
  // RemoveCancelableTask -> UpdateWakeUp -> WakeUpQueue -> SetNextWakeUp ->
  // ScheduleWork -> the WorkDeduplicator's ordered lock and then the pump --
  // mutating ordered state where nothing may be recorded.  Dooming the task
  // above is what actually cancels it; removing it from the heap is only an
  // optimisation, and Task::IsCanceled()/WillRunTask() both consult
  // WasInvalidated(), so a doomed task left in the queue is discarded at its
  // run time with events allowed.  Skip the removal and leak the queue slot,
  // which is the same trade ShutdownTaskQueueGracefully, ~SVGImage,
  // Core::Close and some twenty other leak-references guards already make.
  //
  // The guard is here rather than in ~TimerBase deliberately: the base
  // TimerBase closure is bound with Unretained(this) (base/timer/timer.h),
  // and only HeapTaskRunnerTimer carries a WeakPersistent, so skipping the
  // cancel at the timer would be a use-after-free.  Here it is safe and covers
  // every DelayedTaskHandle holder.  DSRC-384.
  if (recordreplay::AreEventsDisallowed(
          "DelayedTaskHandleDelegate::CancelTask") &&
      recordreplay::EnterLeakMemory("DelayedTaskHandleDelegate::CancelTask")) {
    return;
  }

  // If the task is still inside the heap, then it can be removed directly.
  if (heap_handle_.IsValid()) {
    outer_->RemoveCancelableTask(heap_handle_);
  }
}

void DelayedTaskHandleDelegate::SetHeapHandle(HeapHandle heap_handle) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(heap_handle.IsValid());
  heap_handle_ = heap_handle;
}

void DelayedTaskHandleDelegate::ClearHeapHandle() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  heap_handle_ = HeapHandle();
}

HeapHandle DelayedTaskHandleDelegate::GetHeapHandle() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return heap_handle_;
}

void DelayedTaskHandleDelegate::WillRunTask() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(IsValid());
  // The task must be removed from the heap before running it.
  DCHECK(!heap_handle_.IsValid());

  // Use InvalidateWeakPtrsAndDoom() to avoid allocating a new
  // WeakReference::Flag, as no new WeakPtrs will ever be issued from this
  // delegate.
  weak_ptr_factory_.InvalidateWeakPtrsAndDoom();
}

}  // namespace base::sequence_manager::internal
