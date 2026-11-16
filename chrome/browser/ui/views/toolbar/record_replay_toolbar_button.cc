// Copyright 2023 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/browser_web_contents_delegate/browser_web_contents_delegate.h"
#include "content/public/browser/navigation_controller.h"
#include "base/memory/raw_ptr.h"
#include <string_view>
#include <iostream>

#include "chrome/browser/ui/views/toolbar/record_replay_toolbar_button.h"

#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/webui_url_constants.h"
#include "ui/views/controls/button/button_controller.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

struct RecordReplayToolbarButtonWebContentsObserver
  : public content::WebContentsObserver
{
  RecordReplayToolbarButtonWebContentsObserver(
    content::WebContents* web_contents,
    RecordReplayToolbarButton* button)
    : content::WebContentsObserver(web_contents),
      button_(button)
  {}

  void WebContentsDestroyed() override {
    // Tell toolbar button that recording tab has closed.
    // This will detach the observer from the button.
    button_->RecordingTabDestroyed();
  }

  raw_ptr<RecordReplayToolbarButton> button_;
};

RecordReplayToolbarButton::RecordReplayToolbarButton(
    BrowserWindowInterface* browser)
    : ToolbarButton(base::BindRepeating(&RecordReplayToolbarButton::ButtonPressed,
                                        base::Unretained(this))),
      browser_(browser),
      web_contents_(nullptr),
      web_contents_observer_(nullptr),
      recordreplay_contents_(nullptr) {
  SetVectorIcons(kRecordReplayIcon, kRecordReplayIcon);
  button_controller()->set_notify_action(
      views::ButtonController::NotifyAction::kOnPress);
}

RecordReplayToolbarButton::~RecordReplayToolbarButton() = default;

void RecordReplayToolbarButton::OnPaintBackground(gfx::Canvas* canvas) {
  if (!recordreplay_contents_) {
    CreateHiddenWebContents();
  }

  ToolbarButton::OnPaintBackground(canvas);
}

void RecordReplayToolbarButton::CreateHiddenWebContents() {
  // create a hidden webcontents to run the business logic
  content::WebContents::CreateParams recordreplay_params(
    browser_->GetProfile(),
    content::SiteInstance::CreateForURL(
      browser_->GetProfile(),
      GURL(std::string_view(chrome::kChromeUIRecordReplayPageURL))
    )
  );
  // Upstream removed CreateParams::is_never_visible with no replacement.
  // This WebContents is never attached to a tab strip, so it stays offscreen
  // regardless -- see the defect entry for what that does NOT guarantee.

  recordreplay_contents_ = content::WebContents::Create(recordreplay_params);
  recordreplay_contents_->SetDelegate(
      BrowserWebContentsDelegate::From(browser_));
  recordreplay_contents_->GetController().LoadURL(
    GURL(std::string_view(chrome::kChromeUIRecordReplayPageURL)),
    content::Referrer(),
    ui::PAGE_TRANSITION_TYPED, // wrong.  the user didn't type this
    std::string()
  );
}

void RecordReplayToolbarButton::ButtonPressed() {
  // Check to see if we're currently recording a tab.
  if (web_contents_) {
    // We are recording a tab, stop recording.
    StopRecording();
  } else {
    // We are not recording a tab, start recording.
    StartRecording();
  }
  RefreshIconState();
}

void RecordReplayToolbarButton::StartRecording() {
  // Get the current active tab.  This provides the URL we'll be recording.
  TabStripModel* tab_strip_model = browser_->GetTabStripModel();
  content::WebContents* old_web_contents = tab_strip_model->GetActiveWebContents();
  if (!old_web_contents) {
    return;
  }
  content::BrowserContext* browser_context = old_web_contents->GetBrowserContext();

  GURL url = old_web_contents->GetVisibleURL();

  // Create a new recording web-contents.
  CHECK(!web_contents_);
  CHECK(!web_contents_observer_.get());
  content::WebContents::CreateParams new_params(browser_context);
  new_params.record_replay_for_recording = true;
  std::unique_ptr<content::WebContents> new_web_contents(
    content::WebContents::Create(new_params));
  web_contents_ = new_web_contents.get();

  // Create and add observer.
  // This will notify the button when the recording tab is closed.
  // See: `RecordingTabDestroyed()` below.
  web_contents_observer_ =
    std::make_unique<RecordReplayToolbarButtonWebContentsObserver>(
      web_contents_, this);

  tab_strip_model->AppendWebContents(std::move(new_web_contents), true);

  // Get the index of the old non-recording web-contents, to close it.
  int index = tab_strip_model->GetIndexOfWebContents(old_web_contents);

  // Close old tab and replace with new tab.
  tab_strip_model->CloseWebContentsAt(index, TabCloseTypes::CLOSE_NONE);

  web_contents_->GetController().LoadURL(url, content::Referrer(),
                                         ui::PAGE_TRANSITION_TYPED,
                                         std::string());
}

void RecordReplayToolbarButton::StopRecording() {
  // There should be a recording web-contents.
  if (!web_contents_) {
    // TODO: Print a warning here.  `StopRecording` called when
    // no active recording present.
    return;
  }

  TabStripModel* tab_strip_model = browser_->GetTabStripModel();
  int index = tab_strip_model->GetIndexOfWebContents(web_contents_);
  if (index == TabStripModel::kNoTab) {
    // TODO: Print a warning.  The web contents we have a reference to
    // is not listed in any tab.
    web_contents_ = nullptr;
    web_contents_observer_.release();
    return;
  }

  index = tab_strip_model->GetIndexOfWebContents(web_contents_);
  tab_strip_model->CloseWebContentsAt(index, TabCloseTypes::CLOSE_NONE);
}

void RecordReplayToolbarButton::RecordingTabDestroyed() {
  if (web_contents_) {
    CreatePostRecordingWebContents();
    web_contents_ = nullptr;
    web_contents_observer_.release();
  }
  RefreshIconState();
}

void RecordReplayToolbarButton::RefreshIconState() {
  if (web_contents_) {
    SetVectorIcons(kRecordReplayStopIcon, kRecordReplayStopIcon);
  } else {
    SetVectorIcons(kRecordReplayIcon, kRecordReplayIcon);
  }
}

void RecordReplayToolbarButton::CreatePostRecordingWebContents() {
  content::WebContents::CreateParams new_params(web_contents_->GetBrowserContext());
  std::unique_ptr<content::WebContents> post_recording_web_contents(
    content::WebContents::Create(new_params));
  TabStripModel* tab_strip_model = browser_->GetTabStripModel();

  // TODO: Make this URL point to `app.replay.io` URL for the recording.
  // TODO: this should also maybe be done via WebContents::CreateParams if it can?
  GURL url = GURL("https://app.replay.io");
  post_recording_web_contents->GetController().LoadURL(url, content::Referrer(),
                                              ui::PAGE_TRANSITION_TYPED,
                                              std::string());

  tab_strip_model->AppendWebContents(std::move(post_recording_web_contents), true);
}
BEGIN_METADATA(RecordReplayToolbarButton)
END_METADATA
