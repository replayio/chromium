// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/dom/frame_callback_dependency_scope.h"

namespace blink {

int FrameCallbackDependencyScope::Schedule() {
  return recordreplay::NewDependencyGraphNode(
      "{\"kind\":\"frameCallbackScheduled\"}");
}

FrameCallbackDependencyScope::FrameCallbackDependencyScope(
    int request_node_id) {
  if (!request_node_id)
    return;
  int node_id = recordreplay::NewDependencyGraphNode(
      "{\"kind\":\"frameCallbackFired\"}");
  recordreplay::AddDependencyGraphEdge(request_node_id, node_id,
                                       "{\"kind\":\"frameCallback\"}");
  execute_.emplace(node_id);
}

}  // namespace blink
