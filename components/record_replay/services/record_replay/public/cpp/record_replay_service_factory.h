// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_RECORD_REPLAY_SERVICES_RECORD_REPLAY_PUBLIC_CPP_RECORD_REPLAY_SERVICE_FACTORY_H_
#define COMPONENTS_RECORD_REPLAY_SERVICES_RECORD_REPLAY_PUBLIC_CPP_RECORD_REPLAY_SERVICE_FACTORY_H_

#include "base/no_destructor.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/record_replay/services/record_replay/public/cpp/record_replay_service.h"

namespace content {
class BrowserContext;
}

namespace record_replay {

class RecordReplayService;

// Factory to get or create an instance of RecordReplayService for a
// BrowserContext.
class RecordReplayServiceFactory : public BrowserContextKeyedServiceFactory {
 public:
  static RecordReplayService* GetForBrowserContext(
      content::BrowserContext* context);

 private:
  friend class base::NoDestructor<RecordReplayServiceFactory>;
  static RecordReplayServiceFactory* GetInstance();

  RecordReplayServiceFactory();
  ~RecordReplayServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  KeyedService* BuildServiceInstanceFor(
      content::BrowserContext* context) const override;
  content::BrowserContext* GetBrowserContextToUse(
      content::BrowserContext* context) const override;
};

}  // namespace record_replay

#endif  // COMPONENTS_RECORD_REPLAY_SERVICES_RECORD_REPLAY_PUBLIC_CPP_RECORD_REPLAY_SERVICE_FACTORY_H_
