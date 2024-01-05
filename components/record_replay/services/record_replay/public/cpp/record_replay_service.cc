// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/record_replay/services/record_replay/public/cpp/record_replay_service.h"

#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/service_process_host.h"

#if BUILDFLAG(IS_MAC)
#include <CoreFoundation/CFBundle.h>
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace {
  const char kRecordReplayPrefsKey[] = "record_replay";
}

namespace record_replay {

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(kRecordReplayPrefsKey);
}

RecordReplayService::RecordReplayService(Profile* profile)
  : profile_(profile) {}
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

absl::optional<std::string> RecordReplayService::GetEnv(const std::string& key) {
  return absl::optional<std::string>();
}
std::string RecordReplayService::GetBuildId() {
  return "FIXME-BUILD-ID";
}
absl::optional<std::string> RecordReplayService::GetReplayUserToken() {
  PrefService* prefs = profile_->GetPrefs();
  const std::string* token =
    prefs->GetDict(kRecordReplayPrefsKey).FindStringByDottedPath("user_token");
  if (token) {
    return absl::optional<std::string>(*token);
  }
  return absl::optional<std::string>();
}

void RecordReplayService::SetReplayUserToken(const absl::optional<std::string>& token) {
  ScopedDictPrefUpdate record_replay_prefs(profile_->GetPrefs(), kRecordReplayPrefsKey);
  if (token.has_value()) {
    record_replay_prefs->SetByDottedPath("user_token", token.value());
  } else {
    record_replay_prefs->RemoveByDottedPath("user_token");
  }
}

absl::optional<std::string> RecordReplayService::GetReplayRefreshToken() {
  PrefService* prefs = profile_->GetPrefs();
  const std::string* token =
    prefs->GetDict(kRecordReplayPrefsKey).FindStringByDottedPath("refresh_token");
  if (token) {
    return absl::optional<std::string>(*token);
  }
  return absl::optional<std::string>();
}
void RecordReplayService::SetReplayRefreshToken(const absl::optional<std::string>& token) {
  ScopedDictPrefUpdate record_replay_prefs(profile_->GetPrefs(), kRecordReplayPrefsKey);
  if (token.has_value()) {
    record_replay_prefs->SetByDottedPath("refresh_token", token.value());
  } else {
    record_replay_prefs->Remove("refresh_token");
  }
}
void RecordReplayService::ShowAuthenticationError(const std::string& message) {
  fprintf(stderr, "RecordReplay [RUN-2866] ManagerHandler(%p)::ShowAuthenticationError(%s)\n", this,
         message.c_str());
}

void RecordReplayService::NotifyObservers() {
  for (auto& observer : observers_) {
    observer->OnRecordReplayAuthTokenChanged(token_);
  }
}

#if BUILDFLAG(IS_MAC)
static void OpenExternalBrowserMac(const std::string& url_str) {
  CFURLRef url = CFURLCreateWithBytes (
      NULL,                        // allocator
      (UInt8*)url_str.c_str(),     // URLBytes
      url_str.length(),            // length
      kCFStringEncodingASCII,      // encoding
      NULL                         // baseURL
    );
  LSOpenCFURLRef(url,0);
  CFRelease(url);
}
#endif

#if BUILDFLAG(IS_LINUX)
static void OpenExternalBrowserLinux(const std::string& url_str) {
  std::string cmd = "xdg-open '" + url_str + "'";
  int result = system(cmd.c_str());
  if (result != 0) {
    fprintf(stderr, "RecordReplayManagerHandler::OpenExternalBrowserLinux() failed with %d\n",
           result);
  }
}
#endif

#if BUILDFLAG(IS_WIN)
static void OpenExternalBrowserWindows(const std::string& url_str) {
  fprintf(stderr, "RecordReplayManagerHandler::OpenExternalBrowserWindows() NOT IMPLEMENTED\n");
}
#endif

void RecordReplayService::OpenExternalBrowser(const std::string& url) {
  fprintf(stderr, "RecordReplay [RUN-2866] ManagerHandler(%p)::OpenExternalBrowser(%s)\n", this,
         url.c_str());
  #if BUILDFLAG(IS_MAC)
    OpenExternalBrowserMac(url);
  #elif BUILDFLAG(IS_LINUX)
    OpenExternalBrowserLinux(url);
  #elif BUILDFLAG(IS_WIN)
    OpenExternalBrowserWindows(url);
  #endif
}

}  // namespace record_replay
