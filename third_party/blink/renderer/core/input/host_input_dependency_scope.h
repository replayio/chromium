// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INPUT_HOST_INPUT_DEPENDENCY_SCOPE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INPUT_HOST_INPUT_DEPENDENCY_SCOPE_H_

#include "base/record_replay.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/public/mojom/frame/user_activation_notification_type.mojom-blink-forward.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"

namespace blink {

class WebInputEvent;

// Dependency graph node hostInput{operation} spanning a step the browser takes
// on user input: the handling of a host input event, or a sub-step outside any
// event dispatch.
class CORE_EXPORT HostInputDependencyScope {
  STACK_ALLOCATED();

 public:
  // Sub-steps outside any event dispatch. Inside a host input step, they nest
  // in it.
  enum class Operation {
    // The focus change of a mouse or tap press.
    kMouseFocus,
    // The edit of an IME composition update.
    kCompositionUpdate,
  };

  explicit HostInputDependencyScope(Operation);
  // The step of a host input event a widget handles, with the event's typed
  // fields; nothing for an event which is no step. A step which starts an
  // input action takes a bookmark, the action's point, also while recording.
  explicit HostInputDependencyScope(const WebInputEvent&);
  HostInputDependencyScope(const HostInputDependencyScope&) = delete;
  HostInputDependencyScope& operator=(const HostInputDependencyScope&) = delete;

  // Leaf node userActivation{notificationType}: a frame was notified of a user
  // activation.
  static void RecordUserActivation(
      mojom::blink::UserActivationNotificationType notification_type);

 private:
  absl::optional<recordreplay::AutoDependencyExecution> execute_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INPUT_HOST_INPUT_DEPENDENCY_SCOPE_H_
