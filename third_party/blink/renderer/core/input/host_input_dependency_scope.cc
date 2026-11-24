// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/input/host_input_dependency_scope.h"

#include <string>

#include "base/json/json_writer.h"
#include "base/values.h"

namespace blink {

namespace {

const char* OperationName(HostInputDependencyScope::Operation operation) {
  switch (operation) {
    case HostInputDependencyScope::Operation::kMouseFocus:
      return "mouseFocus";
    case HostInputDependencyScope::Operation::kCompositionUpdate:
      return "compositionUpdate";
  }
}

}  // namespace

HostInputDependencyScope::HostInputDependencyScope(Operation operation) {
  if (!recordreplay::DependencyGraphEnabled())
    return;
  base::Value::Dict info;
  info.Set("kind", "hostInput");
  info.Set("operation", OperationName(operation));
  std::string json;
  base::JSONWriter::Write(info, &json);
  execute_.emplace(recordreplay::NewDependencyGraphNode(json.c_str()));
}

}  // namespace blink
