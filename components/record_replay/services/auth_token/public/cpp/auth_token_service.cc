// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/record_replay/services/auth_token/public/cpp/auth_token_service.h"

#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/service_process_host.h"

namespace {
  const char kRecordReplayPrefsKey[] = "record_replay";
}

namespace auth_token {

RecordReplayAuthTokenService::RecordReplayAuthTokenService(Profile* profile)
  : profile_(profile) {}
RecordReplayAuthTokenService::~RecordReplayAuthTokenService() = default;

void RecordReplayAuthTokenService::BindAuthTokenStore(
    mojo::PendingReceiver<mojom::RecordReplayAuthTokenStore> store) {

  auth_token_stores_.Add(this, std::move(store));
}

void RecordReplayAuthTokenService::SetUserToken(const std::string& user_token) {
  ScopedDictPrefUpdate record_replay_prefs(profile_->GetPrefs(),
                                           kRecordReplayPrefsKey);
  record_replay_prefs->SetByDottedPath("user_token", user_token);
  user_token_ = user_token;
  NotifyObserversAboutUserToken();
}

void RecordReplayAuthTokenService::SetRefreshToken(const std::string& refresh_token) {
  ScopedDictPrefUpdate record_replay_prefs(profile_->GetPrefs(),
                                           kRecordReplayPrefsKey);
  record_replay_prefs->SetByDottedPath("refresh_token", refresh_token);
  refresh_token_ = refresh_token;
  NotifyObserversAboutRefreshToken();
}

void RecordReplayAuthTokenService::AddObserver(mojo::PendingRemote<mojom::RecordReplayAuthTokenStoreObserver> observer) {
  observers_.Add(std::move(observer));
  NotifyObserversAboutUserToken();
  NotifyObserversAboutRefreshToken();
}

void RecordReplayAuthTokenService::NotifyObserversAboutUserToken() {
  for (auto& observer : observers_) {
    observer->OnRecordReplayAuthTokenChanged(user_token_);
  }
}

void RecordReplayAuthTokenService::NotifyObserversAboutRefreshToken() {
  for (auto& observer : observers_) {
    observer->OnRecordReplayRefreshTokenChanged(refresh_token_);
  }
}

}  // namespace auth_token
