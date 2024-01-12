// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/record_replay/record_replay_manager_handler.h"
#include "components/record_replay/services/record_replay/public/cpp/record_replay_service_factory.h"
#include <string>

#include "base/bind.h"
#include "chrome/browser/profiles/profile.h"
#if BUILDFLAG(IS_MAC)
#include <CoreFoundation/CFBundle.h>
#include <ApplicationServices/ApplicationServices.h>
#endif

RecordReplayManagerHandler::RecordReplayManagerHandler(
    Profile* profile,
    mojo::PendingReceiver<mojom::RecordReplayManagerHandler> receiver)
    : profile_(profile),
      service_(record_replay::RecordReplayServiceFactory::GetForBrowserContext(profile_)),
      receiver_(this, std::move(receiver))
{}

RecordReplayManagerHandler::~RecordReplayManagerHandler() = default;

void RecordReplayManagerHandler::SetManager(
    mojo::PendingRemote<mojom::RecordReplayManager> manager) {
  manager_.Bind(std::move(manager));
  manager_->HandleSignInButtonClicked();
}

void RecordReplayManagerHandler::HandleSignInButtonClicked() {
  CHECK(manager_)
    << ("RecordReplayManagerHandler::HandleSignInButtonClicked called"
       " before manager was set");
  manager_->HandleSignInButtonClicked();
}

void RecordReplayManagerHandler::GetEnv(const std::string& key, GetEnvCallback callback) {
  std::move(callback).Run(service_->GetEnv(key));
}
void RecordReplayManagerHandler::GetBuildId(GetBuildIdCallback callback) {
  std::move(callback).Run(service_->GetBuildId());
}
void RecordReplayManagerHandler::GetReplayUserToken(GetReplayUserTokenCallback callback) {
  std::move(callback).Run(service_->GetReplayUserToken());
}
void RecordReplayManagerHandler::SetReplayUserToken(const absl::optional<std::string>& token) {
  service_->SetReplayUserToken(token);
}
void RecordReplayManagerHandler::GetReplayRefreshToken(GetReplayRefreshTokenCallback callback) {
  std::move(callback).Run(service_->GetReplayRefreshToken());
}
void RecordReplayManagerHandler::SetReplayRefreshToken(const absl::optional<std::string>& token) {
  service_->SetReplayRefreshToken(token);
}
void RecordReplayManagerHandler::ShowAuthenticationError(const std::string& message) {
  service_->ShowAuthenticationError(message);
}

void RecordReplayManagerHandler::OpenExternalBrowser(const std::string& url) {
  service_->OpenExternalBrowser(url);
}
