// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/record_replay/services/record_replay/public/cpp/record_replay_service.h"
#include "content/public/browser/service_process_host.h"

namespace record_replay {

RecordReplayService::RecordReplayService() = default;
RecordReplayService::~RecordReplayService() = default;

void RecordReplayService::BindRecordReplayService(
    mojo::PendingReceiver<mojom::RecordReplayService> store) {

  services_.Add(this, std::move(store));
}

void RecordReplayService::SetToken(const std::string& token) {
  token_ = token;
  // TODO persist the token to browser prefs
  NotifyObservers();
}

void RecordReplayService::ClearToken() {
  token_.clear(); // not sure about this..  should we be sending a null?
  NotifyObservers();
}

void RecordReplayService::SetUser(const std::string& user) {
  // TODO persist the user to browser prefs
}

void RecordReplayService::ClearUser() {
  // TODO clear the user from browser prefs
}

void RecordReplayService::Login() {
}

void RecordReplayService::AddObserver(mojo::PendingRemote<mojom::RecordReplayAuthTokenObserver> observer) {
  observers_.Add(std::move(observer));
  NotifyObservers();
}

void RecordReplayService::NotifyObservers() {
  for (auto& observer : observers_) {
    observer->OnRecordReplayAuthTokenChanged(token_);
  }
}

}  // namespace record_replay
