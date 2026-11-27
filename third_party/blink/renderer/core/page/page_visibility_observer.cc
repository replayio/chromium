// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/page/page_visibility_observer.h"

#include "third_party/blink/renderer/core/page/page.h"

namespace blink {

PageVisibilityObserver::PageVisibilityObserver(Page* page) {
  // Pointer registration is needed for sorting in Page::SetVisibilityState
  recordreplay::RegisterPointer("PageVisibilityObserver", this);

  SetPage(page);
}

PageVisibilityObserver::~PageVisibilityObserver() {
  recordreplay::UnregisterPointer(this);
}

void PageVisibilityObserver::ObserverSetWillBeCleared() {
  page_ = nullptr;
}

void PageVisibilityObserver::SetPage(Page* page) {
  if (page == page_)
    return;

  // The page also tracks its observers while recording or replaying, so moving
  // to another page has to release the old page's entry.
  if (page_) {
    page_->PageVisibilityObserverSet().erase(this);
    page_->RecordReplayReleaseVisibilityObserver(this);
  }

  page_ = page;

  if (page_) {
    page_->PageVisibilityObserverSet().insert(this);
    page_->RecordReplayRetainVisibilityObserver(this);
  }
}

void PageVisibilityObserver::Trace(Visitor* visitor) const {
  visitor->Trace(page_);
}

}  // namespace blink
