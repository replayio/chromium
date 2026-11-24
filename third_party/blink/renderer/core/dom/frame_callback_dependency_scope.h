// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_DOM_FRAME_CALLBACK_DEPENDENCY_SCOPE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_DOM_FRAME_CALLBACK_DEPENDENCY_SCOPE_H_

#include "base/record_replay.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"

namespace blink {

// Dependency graph node frameCallbackFired <-frameCallback-
// frameCallbackScheduled, spanning the callback's invocation.
class CORE_EXPORT FrameCallbackDependencyScope {
  STACK_ALLOCATED();

 public:
  // Request node, created by the requester.
  static int Schedule();

  // Node ids are >= 1. Request node id 0 = no request node: no fire node.
  explicit FrameCallbackDependencyScope(int request_node_id);
  FrameCallbackDependencyScope(const FrameCallbackDependencyScope&) = delete;
  FrameCallbackDependencyScope& operator=(const FrameCallbackDependencyScope&) =
      delete;

 private:
  absl::optional<recordreplay::AutoDependencyExecution> execute_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_DOM_FRAME_CALLBACK_DEPENDENCY_SCOPE_H_
