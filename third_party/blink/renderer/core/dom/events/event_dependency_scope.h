// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_DOM_EVENTS_EVENT_DEPENDENCY_SCOPE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_DOM_EVENTS_EVENT_DEPENDENCY_SCOPE_H_

#include "base/record_replay.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"

namespace blink {

class Event;

// Dependency graph node {kind, type} spanning the innermost event dispatch.
class CORE_EXPORT EventDependencyScope {
  STACK_ALLOCATED();

 public:
  enum class Kind {
    // Dispatch to a Node, incl. its default actions. Always gets a node.
    kDispatchEvent,
    // Dispatch to non-Node targets. Gets a node when it first attempts to
    // invoke a script listener; that invocation may still run no script.
    // Nodes created in the dispatch before then stay under the enclosing
    // execution.
    kEmitEvent,
  };

  EventDependencyScope(Kind, const Event&);
  EventDependencyScope(const EventDependencyScope&) = delete;
  EventDependencyScope& operator=(const EventDependencyScope&) = delete;
  ~EventDependencyScope();

  static void WillInvokeScriptListener();

 private:
  void Begin();

  const Kind kind_;
  AtomicString type_;
  bool entered_ = false;
  EventDependencyScope* outer_ = nullptr;
  absl::optional<recordreplay::AutoDependencyExecution> execute_;
};

// Dependency graph node eventQueueDispatch <-scheduler- eventQueueSchedule.
class CORE_EXPORT EventQueueDispatchScope {
  STACK_ALLOCATED();

 public:
  static int Schedule();
  // Schedule() iff there is a cause execution, else 0: a host-queued event
  // keeps its top-level dispatch.
  static int ScheduleIfCaused();

  // Node ids are >= 1. Schedule node id 0 = no schedule node: no dispatch node.
  explicit EventQueueDispatchScope(int schedule_node_id);
  EventQueueDispatchScope(const EventQueueDispatchScope&) = delete;
  EventQueueDispatchScope& operator=(const EventQueueDispatchScope&) = delete;

 private:
  absl::optional<recordreplay::AutoDependencyExecution> execute_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_DOM_EVENTS_EVENT_DEPENDENCY_SCOPE_H_
