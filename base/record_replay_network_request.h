// Copyright (c) 2026 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The requestId of dependency graph nodes consuming a network response.

#ifndef BASE_RECORD_REPLAY_NETWORK_REQUEST_H_
#define BASE_RECORD_REPLAY_NETWORK_REQUEST_H_

#include <cstdint>
#include <string>

#include "base/check.h"
#include "base/strings/string_piece.h"
#include "base/values.h"

namespace recordreplay {

// The network request whose response a dependency graph node consumes.
class ConsumedNetworkRequest {
 public:
  enum class Kind {
    // The request with a Blink inspector identifier.
    kRequest,
    // No request.
    kNone,
    // Not known where the node is created.
    kUnknown,
  };

  static ConsumedNetworkRequest Request(uint64_t inspector_id) {
    return ConsumedNetworkRequest(Kind::kRequest, inspector_id);
  }
  static ConsumedNetworkRequest None() {
    return ConsumedNetworkRequest(Kind::kNone, 0);
  }
  static ConsumedNetworkRequest Unknown() {
    return ConsumedNetworkRequest(Kind::kUnknown, 0);
  }

  Kind kind() const { return kind_; }
  uint64_t inspector_id() const {
    DCHECK(kind_ == Kind::kRequest);
    return inspector_id_;
  }

 private:
  ConsumedNetworkRequest(Kind kind, uint64_t inspector_id)
      : kind_(kind), inspector_id_(inspector_id) {}

  Kind kind_;
  uint64_t inspector_id_;
};

// Names the request with a Blink inspector identifier for its consumers, as
// its latest networkRequest or networkReceiveData node does: called at each.
// Main thread only.
void RegisterNetworkRequestId(uint64_t inspector_id,
                              const std::string& request_id);

// A Replay-internal request: the graph has no vertex for it. Main thread only.
void RegisterReplayInternalNetworkRequest(uint64_t inspector_id);

// Sets a consumer node's requestId: the request's id; null if it consumes no
// request of the graph; absent if unknown, including off the main thread.
void SetConsumedRequestId(base::Value::Dict& info,
                          ConsumedNetworkRequest request);

// As above, for another field naming a request.
void SetConsumedRequestId(base::Value::Dict& info,
                          base::StringPiece key,
                          ConsumedNetworkRequest request);

}  // namespace recordreplay

#endif  // BASE_RECORD_REPLAY_NETWORK_REQUEST_H_
