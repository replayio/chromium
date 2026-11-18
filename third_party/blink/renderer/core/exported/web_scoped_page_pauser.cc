// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/public/platform/web_scoped_page_pauser.h"

#include "base/feature_list.h"
#include "base/record_replay.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/page/scoped_browsing_context_group_pauser.h"
#include "third_party/blink/renderer/core/page/scoped_page_pauser.h"

namespace blink {

WebScopedPagePauser::WebScopedPagePauser(WebLocalFrameImpl& frame) {
  Page* page = WebFrame::ToCoreFrame(frame)->GetPage();
  CHECK(page);
  if (base::FeatureList::IsEnabled(
          features::kPausePagesPerBrowsingContextGroup)) {
    browsing_context_group_pauser_ =
        std::make_unique<ScopedBrowsingContextGroupPauser>(*page);
  } else {
    // Clear the page if we aren't showing the hud display.
    //
    // [RecordReplay] The paused-page HUD is also kept off while recording or
    // replaying. It is new since the fork's M108 baseline, where no part of
    // this path existed, and it makes every compositor frame drawn while a
    // page is paused emit one more text blob: Page::SetShowPausedHudOverlay
    // leads to cc::LayerTreeDebugState::debugger_paused, which makes
    // ShouldCreateHudLayer() true, and HeadsUpDisplayLayerImpl::
    // DrawDebuggerPaused then draws through SkTextBlob. How many frames the
    // impl thread draws during a nested pause loop is not something the
    // recording pins down, so a process-global Skia counter ends up at
    // different values on the two sides.
    if (!base::FeatureList::IsEnabled(
            features::kShowHudDisplayForPausedPages) ||
        recordreplay::IsRecordingOrReplaying("paused-page-hud")) {
      page = nullptr;
    }
    page_pauser_ = std::make_unique<ScopedPagePauser>(page);
  }
}

WebScopedPagePauser::~WebScopedPagePauser() = default;

}  // namespace blink
