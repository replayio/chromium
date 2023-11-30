// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/record_replay/services/auth_token/public/cpp/auth_token_service_factory.h"

#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "content/public/browser/browser_context.h"

namespace auth_token {

// static
auth_token::AuthTokenService*
AuthTokenServiceFactory::GetForBrowserContext(
    content::BrowserContext* context) {
  return static_cast<auth_token::AuthTokenService*>(
      GetInstance()->GetServiceForBrowserContext(context, /*create=*/true));
}

// static
AuthTokenServiceFactory* AuthTokenServiceFactory::GetInstance() {
  static base::NoDestructor<AuthTokenServiceFactory> instance;
  return instance.get();
}

AuthTokenServiceFactory::AuthTokenServiceFactory()
    : BrowserContextKeyedServiceFactory(
          "AuthTokenService",
          BrowserContextDependencyManager::GetInstance()) {}

AuthTokenServiceFactory::~AuthTokenServiceFactory() = default;

KeyedService* AuthTokenServiceFactory::BuildServiceInstanceFor(
    content::BrowserContext* /*context*/) const {
  return new auth_token::AuthTokenService();
}

// Incognito profiles should use their own instance.
content::BrowserContext* AuthTokenServiceFactory::GetBrowserContextToUse(
    content::BrowserContext* context) const {
  return context;
}

}  // namespace screen_ai
