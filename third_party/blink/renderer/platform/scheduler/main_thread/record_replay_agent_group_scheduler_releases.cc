// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/scheduler/main_thread/record_replay_agent_group_scheduler_releases.h"

#include "base/numerics/safe_conversions.h"
#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/scheduler/main_thread/agent_group_scheduler_impl.h"
#include "third_party/blink/renderer/platform/scheduler/main_thread/main_thread_scheduler_impl.h"
#include "third_party/blink/renderer/platform/scheduler/main_thread/main_thread_task_queue.h"

namespace blink {
namespace scheduler {

RecordReplayAgentGroupSchedulerReleases::
    RecordReplayAgentGroupSchedulerReleases(
        MainThreadSchedulerImpl& main_thread_scheduler)
    : main_thread_scheduler_(main_thread_scheduler) {
  if (recordreplay::IsReplaying()) {
    alive_ = MakeGarbageCollected<
        GCedHeapHashMap<int, Member<AgentGroupSchedulerImpl>>>();
  }
}

void RecordReplayAgentGroupSchedulerReleases::OnAdded(
    AgentGroupSchedulerImpl& scheduler) {
  int id = scheduler.RecordReplayId();
  if (!id) {
    return;
  }
  task_queues_.insert(id, TaskQueues{scheduler.default_task_queue_,
                                     scheduler.compositor_task_queue_});
  if (alive_) {
    alive_->insert(id, &scheduler);
  }
}

bool RecordReplayAgentGroupSchedulerReleases::OnCollected(int scheduler_id) {
  if (!scheduler_id) {
    return false;
  }
  if (recordreplay::IsRecording()) {
    collected_.push_back(scheduler_id);
  }
  return true;
}

void RecordReplayAgentGroupSchedulerReleases::Poll() {
  Vector<int> ids;
  ids.swap(collected_);
  ids.resize(base::checked_cast<wtf_size_t>(recordreplay::RecordReplayValue(
      "AgentGroupSchedulers collected", ids.size())));
  recordreplay::RecordReplayBytes("AgentGroupSchedulers collected", ids.data(),
                                  ids.size() * sizeof(int));

  for (int id : ids) {
    TaskQueues task_queues = task_queues_.Take(id);
    if (!task_queues.default_task_queue) {
      continue;
    }
    // What AgentGroupSchedulerImpl::Dispose() does upstream.
    task_queues.default_task_queue->DetachTaskQueue();
    task_queues.compositor_task_queue->DetachTaskQueue();
    if (alive_) {
      main_thread_scheduler_->RecordReplayForgetAgentGroupScheduler(
          alive_->Take(id));
    }
  }
}

}  // namespace scheduler
}  // namespace blink
