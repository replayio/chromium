// Copyright (c) 2026 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/record_replay_network_request.h"

#include <unordered_map>
#include <unordered_set>

namespace recordreplay {

extern bool IsMainThread();

// Kept past the request's finish: consumers run after it.
static std::unordered_map<uint64_t, std::string>& NetworkRequestIds() {
  static auto* request_ids = new std::unordered_map<uint64_t, std::string>();
  return *request_ids;
}

static std::unordered_set<uint64_t>& ReplayInternalNetworkRequests() {
  static auto* requests = new std::unordered_set<uint64_t>();
  return *requests;
}

void RegisterNetworkRequestId(uint64_t inspector_id,
                              const std::string& request_id) {
  DCHECK(IsMainThread());
  NetworkRequestIds()[inspector_id] = request_id;
}

void RegisterReplayInternalNetworkRequest(uint64_t inspector_id) {
  DCHECK(IsMainThread());
  ReplayInternalNetworkRequests().insert(inspector_id);
}

void SetConsumedRequestId(base::Value::Dict& info,
                          ConsumedNetworkRequest request) {
  SetConsumedRequestId(info, "requestId", request);
}

void SetConsumedRequestId(base::Value::Dict& info,
                          base::StringPiece key,
                          ConsumedNetworkRequest request) {
  switch (request.kind()) {
    case ConsumedNetworkRequest::Kind::kNone:
      info.Set(key, base::Value());
      return;
    case ConsumedNetworkRequest::Kind::kUnknown:
      return;
    case ConsumedNetworkRequest::Kind::kRequest:
      break;
  }
  if (!IsMainThread()) {
    return;
  }
  const uint64_t inspector_id = request.inspector_id();
  const auto& request_ids = NetworkRequestIds();
  auto it = request_ids.find(inspector_id);
  if (it != request_ids.end()) {
    info.Set(key, it->second);
  } else if (ReplayInternalNetworkRequests().count(inspector_id)) {
    info.Set(key, base::Value());
  }
}

}  // namespace recordreplay
