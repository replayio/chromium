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

class LocalFrame;
class WebInputEvent;

// Blink state which pairs the start step of an input action with a later step.
enum class InputActionPairing {
  // MouseEventManager press state: mouse press -> release.
  kMousePressRelease,
  // WebFrameWidgetImpl suppress_next_keypress_event_: key down -> char.
  kKeyDownChar,
};

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
  ~HostInputDependencyScope();

  // Leaf node userActivation{notificationType}: a frame was notified of a user
  // activation.
  static void RecordUserActivation(
      mojom::blink::UserActivationNotificationType notification_type);

 private:
  friend class InputActionHolder;

  // The scope of the innermost host input event a widget handles. Main thread
  // only: widgets handle input there.
  static const HostInputDependencyScope* current_event_scope_;

  bool is_event_scope_ = false;
  const HostInputDependencyScope* outer_event_scope_ = nullptr;
  // 0: the event is no step, or the graph is disabled.
  int step_node_id_ = 0;
  // The pairing state the step writes (a start step) or reads (a continuation
  // step).
  absl::optional<InputActionPairing> holds_;
  absl::optional<InputActionPairing> consumes_;
  absl::optional<recordreplay::AutoDependencyExecution> execute_;
};

// The input action id held beside one Blink pairing state, written and dropped
// where Blink writes and clears that state. Recorder data only.
class CORE_EXPORT InputActionHolder {
  DISALLOW_NEW();

 public:
  explicit InputActionHolder(InputActionPairing pairing) : pairing_(pairing) {}
  InputActionHolder(const InputActionHolder&) = delete;
  InputActionHolder& operator=(const InputActionHolder&) = delete;

  // Blink wrote the pairing state: holds the current step's action if the step
  // starts one of this pairing, else none.
  void Hold();
  // Same, for a state each frame keeps: the local root's holds.
  void Hold(const LocalFrame& frame);
  // Blink paired the current step: edge inputActionStep from the held action
  // to the step, if the step continues this pairing.
  void Consume() const;
  // Blink cleared the pairing state.
  void Drop() { action_node_id_ = 0; }

 private:
  const InputActionPairing pairing_;
  int action_node_id_ = 0;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INPUT_HOST_INPUT_DEPENDENCY_SCOPE_H_
