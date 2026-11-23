// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/dom/events/event_dependency_scope.h"

#include <string>

#include "base/json/json_writer.h"
#include "base/values.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"

namespace blink {

namespace {

// Main thread only: set iff the dependency graph is enabled.
EventDependencyScope* g_innermost_scope = nullptr;

const char* KindName(EventDependencyScope::Kind kind) {
  switch (kind) {
    case EventDependencyScope::Kind::kDispatchEvent:
      return "dispatchEvent";
    case EventDependencyScope::Kind::kEmitEvent:
      return "emitEvent";
  }
}

}  // namespace

EventDependencyScope::EventDependencyScope(Kind kind, const Event& event)
    : kind_(kind) {
  if (!recordreplay::DependencyGraphEnabled())
    return;
  type_ = event.type();
  entered_ = true;
  outer_ = g_innermost_scope;
  g_innermost_scope = this;
  if (kind_ == Kind::kDispatchEvent)
    Begin();
}

EventDependencyScope::~EventDependencyScope() {
  if (entered_)
    g_innermost_scope = outer_;
}

void EventDependencyScope::WillInvokeScriptListener() {
  if (!IsMainThread() || !g_innermost_scope || g_innermost_scope->execute_)
    return;
  g_innermost_scope->Begin();
}

void EventDependencyScope::Begin() {
  base::Value::Dict info;
  info.Set("kind", KindName(kind_));
  info.Set("type", type_.Utf8());
  std::string json;
  base::JSONWriter::Write(info, &json);
  execute_.emplace(recordreplay::NewDependencyGraphNode(json.c_str()));
}

int EventQueueDispatchScope::Schedule() {
  return recordreplay::NewDependencyGraphNode(
      "{\"kind\":\"eventQueueSchedule\"}");
}

EventQueueDispatchScope::EventQueueDispatchScope(int schedule_node_id) {
  if (!recordreplay::DependencyGraphEnabled())
    return;
  int node_id = recordreplay::NewDependencyGraphNode(
      "{\"kind\":\"eventQueueDispatch\"}");
  recordreplay::AddDependencyGraphEdge(schedule_node_id, node_id,
                                       "{\"kind\":\"scheduler\"}");
  execute_.emplace(node_id);
}

}  // namespace blink
