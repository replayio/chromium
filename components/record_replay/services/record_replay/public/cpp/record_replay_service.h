// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_RECORD_REPLAY_SERVICES_RECORD_REPLAY_PUBLIC_CPP_RECORD_REPLAY_SERVICE_H_
#define COMPONENTS_RECORD_REPLAY_SERVICES_RECORD_REPLAY_PUBLIC_CPP_RECORD_REPLAY_SERVICE_H_

#include "components/keyed_service/core/keyed_service.h"
#include "components/record_replay/services/record_replay/public/mojom/record_replay.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote_set.h"

namespace record_replay {

class RecordReplayService : public KeyedService, public mojom::RecordReplayService {
 public:
  RecordReplayService();
  RecordReplayService(const RecordReplayService&) = delete;
  RecordReplayService& operator=(const RecordReplayService&) = delete;
  ~RecordReplayService() override;

  void BindRecordReplayService(
      mojo::PendingReceiver<mojom::RecordReplayService> store);

  // mojom::RecordReplayService:
  void SetToken(const std::string& token) override;
  void ClearToken() override;

  void SetUser(const std::string& user) override;
  void ClearUser() override;

  void Login() override;

  void AddObserver(mojo::PendingRemote<mojom::RecordReplayAuthTokenObserver> observer) override;

private:
  mojo::ReceiverSet<mojom::RecordReplayService> services_;

  std::string token_;

  void NotifyObservers();

  mojo::RemoteSet<mojom::RecordReplayAuthTokenObserver> observers_;
};

}  // namespace record_replay

#endif  // COMPONENTS_RECORD_REPLAY_SERVICES_RECORD_REPLAY_PUBLIC_CPP_RECORD_REPLAY_SERVICE_H_
