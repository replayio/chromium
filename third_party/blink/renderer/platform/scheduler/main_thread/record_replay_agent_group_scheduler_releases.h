// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_MAIN_THREAD_RECORD_REPLAY_AGENT_GROUP_SCHEDULER_RELEASES_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_MAIN_THREAD_RECORD_REPLAY_AGENT_GROUP_SCHEDULER_RELEASES_H_

#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_map.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "v8/include/replayio.h"

namespace blink {
namespace scheduler {

class AgentGroupSchedulerImpl;
class MainThreadSchedulerImpl;
class MainThreadTaskQueue;

// Releases agent group schedulers where the GC poll says the recording's GC
// collected them, when recording or replaying.
//
// Upstream, the GC finalizes a scheduler nothing references any more, and its
// pre-finalizer detaches its task queues. The main thread scheduler then checks
// detached queues after every task, under their locks. A replay's collections
// don't happen at the recording's points, so it would take those locks at
// different points and hang.
//
// So, as with FinalizationRegistries (see v8/src/replay/finalization-registry.h),
// the GC only notes what it collected, and the release runs at the poll on
// both sides, from what is kept here for each scheduler. The recording's GC
// notes the schedulers it collects. The replay keeps every scheduler alive, and
// at the poll releases the ones the recording names. The recording's GC still
// drops a scheduler from the main thread scheduler's weak set of agent group
// schedulers. That set is only read after a poll, see
// MainThreadSchedulerImpl::PerformMicrotaskCheckpoint, where the replay drops
// the same schedulers.
//
// The poll does nothing where events are unavailable, so a checkpoint there
// still reads a set the recording's GC may already have changed. Schedulers
// made at a point which does not replay have no id and keep the upstream
// behavior.
class RecordReplayAgentGroupSchedulerReleases final
    : public v8::replayio::ReplayGCPollParticipant {
 public:
  explicit RecordReplayAgentGroupSchedulerReleases(
      MainThreadSchedulerImpl& main_thread_scheduler);

  void OnAdded(AgentGroupSchedulerImpl& scheduler);
  // Called from the scheduler's pre-finalizer. Returns whether the poll
  // releases the scheduler instead.
  bool OnCollected(int scheduler_id);

  // v8::replayio::ReplayGCPollParticipant:
  bool IsActive() const override { return true; }
  bool HasPending() const override { return !collected_.empty(); }
  void Poll() override;

 private:
  struct TaskQueues {
    scoped_refptr<MainThreadTaskQueue> default_task_queue;
    scoped_refptr<MainThreadTaskQueue> compositor_task_queue;
  };

  const raw_ref<MainThreadSchedulerImpl> main_thread_scheduler_;
  // The task queues of every scheduler with an id which has not been released.
  HashMap<int, TaskQueues> task_queues_;
  // When recording: the ids of the schedulers the GC collected since the last
  // poll.
  Vector<int> collected_;
  // When replaying: the schedulers which have not been released.
  Persistent<GCedHeapHashMap<int, Member<AgentGroupSchedulerImpl>>> alive_;
};

}  // namespace scheduler
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_MAIN_THREAD_RECORD_REPLAY_AGENT_GROUP_SCHEDULER_RELEASES_H_
