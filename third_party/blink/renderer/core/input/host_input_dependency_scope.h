// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INPUT_HOST_INPUT_DEPENDENCY_SCOPE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INPUT_HOST_INPUT_DEPENDENCY_SCOPE_H_

#include "base/record_replay.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"

namespace blink {

// Dependency graph node hostInput{operation} spanning a step the browser takes
// on user input outside any event dispatch.
class CORE_EXPORT HostInputDependencyScope {
  STACK_ALLOCATED();

 public:
  enum class Operation {
    // The focus change of a mouse or tap press.
    kMouseFocus,
    // The edit of an IME composition update.
    kCompositionUpdate,
  };

  explicit HostInputDependencyScope(Operation);
  HostInputDependencyScope(const HostInputDependencyScope&) = delete;
  HostInputDependencyScope& operator=(const HostInputDependencyScope&) = delete;

 private:
  absl::optional<recordreplay::AutoDependencyExecution> execute_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INPUT_HOST_INPUT_DEPENDENCY_SCOPE_H_
