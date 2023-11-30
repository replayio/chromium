// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_XXXX_H_
#define COMPONENTS_XXXX_H_

#include "components/keyed_service/core/keyed_service.h"
#include "components/record_replay/services/auth_token/public/mojom/auth_token.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote_set.h"

namespace auth_token {

class AuthTokenService : public KeyedService, public mojom::AuthTokenStore {
 public:
  AuthTokenService();
  AuthTokenService(const AuthTokenService&) = delete;
  AuthTokenService& operator=(const AuthTokenService&) = delete;
  ~AuthTokenService() override;

  void BindAuthTokenStore(
      mojo::PendingReceiver<mojom::AuthTokenStore> store);

  // mojom::AuthTokenStore:
  void SetToken(const std::string& token) override;
  void AddObserver(mojo::PendingRemote<mojom::AuthTokenStoreObserver> observer) override;

private:
  mojo::ReceiverSet<mojom::AuthTokenStore> auth_token_stores_;

  std::string token_;

  void NotifyObservers();

  mojo::RemoteSet<mojom::AuthTokenStoreObserver> observers_;
};

}  // namespace auth_token

#endif  // COMPONENTS_XXXX_H_
