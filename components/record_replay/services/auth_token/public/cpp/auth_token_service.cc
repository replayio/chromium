// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/record_replay/services/auth_token/public/cpp/auth_token_service.h"
#include "content/public/browser/service_process_host.h"

namespace auth_token {

AuthTokenService::AuthTokenService() = default;
AuthTokenService::~AuthTokenService() = default;

void AuthTokenService::BindAuthTokenStore(
    mojo::PendingReceiver<mojom::AuthTokenStore> store) {

  auth_token_stores_.Add(this, std::move(store));
}

void AuthTokenService::SetToken(const std::string& token) {
  token_ = token;
  NotifyObservers();
}

void AuthTokenService::AddObserver(mojo::PendingRemote<mojom::AuthTokenStoreObserver> observer) {
  observers_.Add(std::move(observer));
  NotifyObservers();
}

void AuthTokenService::NotifyObservers() {
  for (auto& observer : observers_) {
    observer->OnTokenChanged(token_);
  }
}

}  // namespace auth_token
