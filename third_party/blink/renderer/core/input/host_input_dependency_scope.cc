// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/input/host_input_dependency_scope.h"

#include <string>

#include "base/json/json_writer.h"
#include "base/numerics/safe_conversions.h"
#include "base/values.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/mojom/frame/user_activation_notification_type.mojom-blink.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/keycode_converter.h"

namespace blink {

namespace {

// The step of a host input event.
enum class StepOperation {
  kPress,
  kRelease,
  kChar,
};

enum class StepClass {
  // Starts an input action.
  kStart,
  // A later step of an input action.
  kContinuation,
};

const char* OperationName(HostInputDependencyScope::Operation operation) {
  switch (operation) {
    case HostInputDependencyScope::Operation::kMouseFocus:
      return "mouseFocus";
    case HostInputDependencyScope::Operation::kCompositionUpdate:
      return "compositionUpdate";
  }
}

const char* StepOperationName(StepOperation operation) {
  switch (operation) {
    case StepOperation::kPress:
      return "press";
    case StepOperation::kRelease:
      return "release";
    case StepOperation::kChar:
      return "char";
  }
}

StepClass StepClassOf(StepOperation operation) {
  switch (operation) {
    case StepOperation::kPress:
      return StepClass::kStart;
    case StepOperation::kRelease:
    case StepOperation::kChar:
      return StepClass::kContinuation;
  }
}

absl::optional<StepOperation> StepOperationOf(const WebInputEvent& event) {
  switch (event.GetType()) {
    case WebInputEvent::Type::kMouseDown:
      // Blink handles no press without a button
      // (EventHandler::HandleMousePressEvent).
      if (static_cast<const WebMouseEvent&>(event).button ==
          WebPointerProperties::Button::kNoButton) {
        return absl::nullopt;
      }
      return StepOperation::kPress;
    case WebInputEvent::Type::kRawKeyDown:
    case WebInputEvent::Type::kKeyDown:
      return StepOperation::kPress;
    case WebInputEvent::Type::kMouseUp:
    case WebInputEvent::Type::kKeyUp:
      return StepOperation::kRelease;
    case WebInputEvent::Type::kChar:
      return StepOperation::kChar;
    default:
      return absl::nullopt;
  }
}

absl::optional<InputActionPairing> PairingOf(const WebInputEvent& event) {
  switch (event.GetType()) {
    case WebInputEvent::Type::kMouseDown:
    case WebInputEvent::Type::kMouseUp:
      return InputActionPairing::kMousePressRelease;
    case WebInputEvent::Type::kRawKeyDown:
    case WebInputEvent::Type::kKeyDown:
    case WebInputEvent::Type::kChar:
      return InputActionPairing::kKeyDownChar;
    default:
      // A key up: Blink keeps no key down -> key up state.
      return absl::nullopt;
  }
}

const char* DeviceName(WebPointerProperties::PointerType pointer_type) {
  switch (pointer_type) {
    case WebPointerProperties::PointerType::kUnknown:
      return "unknown";
    case WebPointerProperties::PointerType::kMouse:
      return "mouse";
    case WebPointerProperties::PointerType::kPen:
      return "pen";
    case WebPointerProperties::PointerType::kTouch:
      return "touch";
    case WebPointerProperties::PointerType::kEraser:
      return "eraser";
  }
}

const char* ButtonName(WebPointerProperties::Button button) {
  switch (button) {
    case WebPointerProperties::Button::kNoButton:
      return "none";
    case WebPointerProperties::Button::kLeft:
      return "left";
    case WebPointerProperties::Button::kMiddle:
      return "middle";
    case WebPointerProperties::Button::kRight:
      return "right";
    case WebPointerProperties::Button::kBack:
      return "back";
    case WebPointerProperties::Button::kForward:
      return "forward";
    case WebPointerProperties::Button::kEraser:
      return "eraser";
  }
}

void SetMouseFields(const WebMouseEvent& event, base::Value::Dict& info) {
  info.Set("device", DeviceName(event.pointer_type));
  info.Set("button", ButtonName(event.button));
}

void SetKeyFields(const WebKeyboardEvent& event,
                  StepOperation operation,
                  base::Value::Dict& info) {
  info.Set("device", "keyboard");
  ui::DomCode code = static_cast<ui::DomCode>(event.dom_code);
  if (code != ui::DomCode::NONE)
    info.Set("code", ui::KeycodeConverter::DomCodeToCodeString(code));
  if (operation == StepOperation::kPress) {
    info.Set("repeat",
             (event.GetModifiers() & WebInputEvent::kIsAutoRepeat) != 0);
  }
}

base::Value::Dict StepInfo(const WebInputEvent& event,
                           StepOperation operation,
                           absl::optional<uint64_t> bookmark) {
  base::Value::Dict info;
  info.Set("kind", "hostInput");
  info.Set("operation", StepOperationName(operation));
  if (WebInputEvent::IsMouseEventType(event.GetType())) {
    SetMouseFields(static_cast<const WebMouseEvent&>(event), info);
  } else {
    SetKeyFields(static_cast<const WebKeyboardEvent&>(event), operation, info);
  }
  if (bookmark)
    info.Set("bookmark", base::checked_cast<int>(*bookmark));
  return info;
}

// The point of an input action. Taken outside the graph gate: a bookmark
// advances the progress counter, which recording and replaying must agree on.
// No events-disallowed guard: the step runs page script, which records events
// itself, as do the protocol OnMouseEvent / OnKeyEvent bookmarks of the step.
absl::optional<uint64_t> NewActionBookmark() {
  if (!recordreplay::IsRecordingOrReplaying())
    return absl::nullopt;
  // 0: V8 takes none, before the first checkpoint or where V8 disallows JS
  // execution.
  uint64_t bookmark = recordreplay::NewBookmark();
  if (!bookmark)
    return absl::nullopt;
  return bookmark;
}

const char* NotificationTypeName(
    mojom::blink::UserActivationNotificationType notification_type) {
  using Type = mojom::blink::UserActivationNotificationType;
  switch (notification_type) {
    case Type::kNone:
      return "none";
    case Type::kInteraction:
      return "interaction";
    case Type::kDevTools:
      return "devTools";
    case Type::kExtensionEvent:
      return "extensionEvent";
    case Type::kExtensionGuestView:
      return "extensionGuestView";
    case Type::kExtensionMessaging:
      return "extensionMessaging";
    case Type::kMedia:
      return "media";
    case Type::kFileSystemAccess:
      return "fileSystemAccess";
    case Type::kPlugin:
      return "plugin";
    case Type::kTest:
      return "test";
    case Type::kWebScriptExec:
      return "webScriptExec";
    case Type::kVoiceSearch:
      return "voiceSearch";
    case Type::kExtensionMessagingBothPrivileged:
      return "extensionMessagingBothPrivileged";
    case Type::kExtensionMessagingSenderPrivileged:
      return "extensionMessagingSenderPrivileged";
    case Type::kExtensionMessagingReceiverPrivileged:
      return "extensionMessagingReceiverPrivileged";
    case Type::kExtensionMessagingNeitherPrivileged:
      return "extensionMessagingNeitherPrivileged";
  }
}

int NewNode(const base::Value::Dict& info) {
  std::string json;
  base::JSONWriter::Write(info, &json);
  return recordreplay::NewDependencyGraphNode(json.c_str());
}

}  // namespace

// static
const HostInputDependencyScope* HostInputDependencyScope::current_event_scope_ =
    nullptr;

HostInputDependencyScope::HostInputDependencyScope(Operation operation) {
  if (!recordreplay::DependencyGraphEnabled())
    return;
  base::Value::Dict info;
  info.Set("kind", "hostInput");
  info.Set("operation", OperationName(operation));
  execute_.emplace(NewNode(info));
}

HostInputDependencyScope::HostInputDependencyScope(const WebInputEvent& event)
    : is_event_scope_(true), outer_event_scope_(current_event_scope_) {
  current_event_scope_ = this;
  absl::optional<StepOperation> operation = StepOperationOf(event);
  if (!operation)
    return;
  StepClass step_class = StepClassOf(*operation);
  absl::optional<uint64_t> bookmark;
  if (step_class == StepClass::kStart)
    bookmark = NewActionBookmark();
  if (!recordreplay::DependencyGraphEnabled())
    return;
  step_node_id_ = NewNode(StepInfo(event, *operation, bookmark));
  if (step_class == StepClass::kStart)
    holds_ = PairingOf(event);
  else
    consumes_ = PairingOf(event);
  execute_.emplace(step_node_id_);
}

HostInputDependencyScope::~HostInputDependencyScope() {
  if (is_event_scope_)
    current_event_scope_ = outer_event_scope_;
}

// static
void HostInputDependencyScope::RecordUserActivation(
    mojom::blink::UserActivationNotificationType notification_type) {
  if (!recordreplay::DependencyGraphEnabled())
    return;
  base::Value::Dict info;
  info.Set("kind", "userActivation");
  info.Set("notificationType", NotificationTypeName(notification_type));
  NewNode(info);
}

void InputActionHolder::Hold() {
  const HostInputDependencyScope* step =
      HostInputDependencyScope::current_event_scope_;
  action_node_id_ =
      (step && step->holds_ == pairing_) ? step->step_node_id_ : 0;
}

void InputActionHolder::Hold(const LocalFrame& frame) {
  // The local root's state alone pairs each press and release of its widget;
  // the step also writes and reads the state of the frames below it
  // (EventHandler::PassMousePressEventToSubframe,
  // EventHandler::PassMouseReleaseEventToSubframe).
  if (frame.IsLocalRoot())
    Hold();
}

void InputActionHolder::Consume() const {
  const HostInputDependencyScope* step =
      HostInputDependencyScope::current_event_scope_;
  if (!action_node_id_ || !step || step->consumes_ != pairing_)
    return;
  recordreplay::AddDependencyGraphEdge(action_node_id_, step->step_node_id_,
                                       "{\"kind\":\"inputActionStep\"}");
}

}  // namespace blink
