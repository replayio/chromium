// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_SVG_SVG_USE_SHADOW_TREE_DEPENDENCY_SCOPE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_SVG_SVG_USE_SHADOW_TREE_DEPENDENCY_SCOPE_H_

#include "base/record_replay.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"

namespace blink {

// Dependency graph node svgUseShadowTreeBuild <-scheduler-
// svgUseShadowTreeScheduled, spanning the shadow tree build of one <use>.
class CORE_EXPORT SVGUseShadowTreeDependencyScope {
  STACK_ALLOCATED();

 public:
  // Schedule node iff there is a cause execution, else 0: the host scheduled
  // the build.
  static int ScheduleIfCaused();

  // Node ids are >= 1. Schedule node id 0 = no schedule node: the build node
  // has no scheduler edge. It is still created: the build's DOM mutations need
  // a cause execution.
  explicit SVGUseShadowTreeDependencyScope(int schedule_node_id);
  SVGUseShadowTreeDependencyScope(const SVGUseShadowTreeDependencyScope&) =
      delete;
  SVGUseShadowTreeDependencyScope& operator=(
      const SVGUseShadowTreeDependencyScope&) = delete;

 private:
  absl::optional<recordreplay::AutoDependencyExecution> execute_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_SVG_SVG_USE_SHADOW_TREE_DEPENDENCY_SCOPE_H_
