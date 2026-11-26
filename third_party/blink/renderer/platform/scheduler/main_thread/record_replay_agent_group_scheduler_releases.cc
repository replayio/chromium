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
  // A scheduler without an id was made at a point which does not replay, and
  // is left to the GC.
  if (recordreplay::IsReplaying() && scheduler.RecordReplayId()) {
    alive_->insert(scheduler.RecordReplayId(), &scheduler);
  }
}

bool RecordReplayAgentGroupSchedulerReleases::DeferDispose(
    int scheduler_id,
    scoped_refptr<MainThreadTaskQueue> default_task_queue,
    scoped_refptr<MainThreadTaskQueue> compositor_task_queue) {
  if (!recordreplay::IsRecording() || !scheduler_id) {
    return false;
  }
  collected_.push_back(Collected{scheduler_id, std::move(default_task_queue),
                                 std::move(compositor_task_queue)});
  return true;
}

void RecordReplayAgentGroupSchedulerReleases::Poll() {
  Vector<int> ids;
  for (const Collected& collected : collected_) {
    ids.push_back(collected.scheduler_id);
  }
  ids.resize(base::checked_cast<wtf_size_t>(recordreplay::RecordReplayValue(
      "AgentGroupSchedulers collected", ids.size())));
  recordreplay::RecordReplayBytes("AgentGroupSchedulers collected", ids.data(),
                                  ids.size() * sizeof(int));

  if (recordreplay::IsRecording()) {
    Vector<Collected> collected;
    collected.swap(collected_);
    for (Collected& scheduler : collected) {
      scheduler.default_task_queue->DetachTaskQueue();
      scheduler.compositor_task_queue->DetachTaskQueue();
    }
    return;
  }

  for (int id : ids) {
    auto it = alive_->find(id);
    if (it == alive_->end()) {
      continue;
    }
    AgentGroupSchedulerImpl* scheduler = it->value;
    alive_->erase(it);
    scheduler->RecordReplayDetachTaskQueues();
    main_thread_scheduler_->RecordReplayForgetAgentGroupScheduler(scheduler);
  }
}

}  // namespace scheduler
}  // namespace blink
