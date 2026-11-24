// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/svg/svg_use_shadow_tree_dependency_scope.h"

namespace blink {

int SVGUseShadowTreeDependencyScope::ScheduleIfCaused() {
  if (!recordreplay::DependencyGraphCauseExecutionNode())
    return 0;
  return recordreplay::NewDependencyGraphNode(
      "{\"kind\":\"svgUseShadowTreeScheduled\"}");
}

SVGUseShadowTreeDependencyScope::SVGUseShadowTreeDependencyScope(
    int schedule_node_id) {
  if (!recordreplay::DependencyGraphEnabled())
    return;
  int node_id = recordreplay::NewDependencyGraphNode(
      "{\"kind\":\"svgUseShadowTreeBuild\"}");
  if (schedule_node_id) {
    recordreplay::AddDependencyGraphEdge(schedule_node_id, node_id,
                                         "{\"kind\":\"scheduler\"}");
  }
  execute_.emplace(node_id);
}

}  // namespace blink
