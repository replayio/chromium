// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/record_replay/services/record_replay/public/cpp/record_replay_service_factory.h"

#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "content/public/browser/browser_context.h"

namespace record_replay {

// static
record_replay::RecordReplayService*
RecordReplayServiceFactory::GetForBrowserContext(
    content::BrowserContext* context) {
  return static_cast<record_replay::RecordReplayService*>(
      GetInstance()->GetServiceForBrowserContext(context, /*create=*/true));
}

// static
RecordReplayServiceFactory* RecordReplayServiceFactory::GetInstance() {
  static base::NoDestructor<RecordReplayServiceFactory> instance;
  return instance.get();
}

RecordReplayServiceFactory::RecordReplayServiceFactory()
    : BrowserContextKeyedServiceFactory(
          "RecordReplayService",
          BrowserContextDependencyManager::GetInstance()) {}

RecordReplayServiceFactory::~RecordReplayServiceFactory() = default;

KeyedService* RecordReplayServiceFactory::BuildServiceInstanceFor(
    content::BrowserContext* /*context*/) const {
  return new record_replay::RecordReplayService();
}

// Incognito profiles should use their own instance.
content::BrowserContext* RecordReplayServiceFactory::GetBrowserContextToUse(
    content::BrowserContext* context) const {
  return context;
}

}  // namespace record_replay
