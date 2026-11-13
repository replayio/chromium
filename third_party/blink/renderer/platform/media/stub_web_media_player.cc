// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/public/platform/media/stub_web_media_player.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/bind.h"
#include "base/cancelable_callback.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "third_party/blink/public/platform/media/webmediaplayer_delegate.h"
#include "third_party/blink/public/platform/web_media_player_client.h"
#include "third_party/blink/public/platform/web_media_player_source.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_time_range.h"
#include "third_party/blink/public/platform/web_url.h"
#include "url/gurl.h"

namespace blink {

namespace {

// Every resource is this long.
constexpr double kDurationSeconds = 10;

constexpr char kUnsupportedSourceMessage[] =
    "RECORD_REPLAY_NO_MEDIA: Only URL sources load while recording or "
    "replaying";

class StubWebMediaPlayer : public WebMediaPlayer,
                           public WebMediaPlayerDelegate::Observer {
 public:
  StubWebMediaPlayer(
      WebMediaPlayerClient* client,
      WebMediaPlayerDelegate* delegate,
      scoped_refptr<base::SingleThreadTaskRunner> main_task_runner)
      : client_(client),
        delegate_(delegate),
        main_task_runner_(std::move(main_task_runner)),
        // An id of its own keeps the browser's player ids unique.
        delegate_id_(delegate_->AddObserver(this)) {}

  ~StubWebMediaPlayer() override { delegate_->RemoveObserver(delegate_id_); }

  // WebMediaPlayer
  LoadTiming Load(LoadType load_type,
                  const WebMediaPlayerSource& source,
                  CorsMode,
                  bool is_cache_disabled) override {
    if (load_type != kLoadTypeURL) {
      PostTask(&StubWebMediaPlayer::FailLoad);
      return LoadTiming::kImmediate;
    }
    url_ = GURL(source.GetAsURL());
    SetNetworkState(kNetworkStateLoading);
    PostTask(&StubWebMediaPlayer::FinishLoad);
    return LoadTiming::kImmediate;
  }

  void Play() override {
    Rebase();
    paused_ = false;
    ScheduleEnd();
  }

  void Pause() override {
    Rebase();
    paused_ = true;
    end_.Cancel();
  }

  void Seek(double seconds) override {
    position_ = std::clamp(seconds, 0.0, kDurationSeconds);
    ended_ = false;
    seeking_ = true;
    end_.Cancel();
    seek_done_.Reset(base::BindOnce(&StubWebMediaPlayer::FinishSeek,
                                    base::Unretained(this)));
    main_task_runner_->PostTask(FROM_HERE, seek_done_.callback());
  }

  void SetRate(double rate) override {
    Rebase();
    rate_ = rate;
    ScheduleEnd();
  }

  void SetVolume(double) override {}
  void SetLatencyHint(double) override {}
  void SetPreservesPitch(bool) override {}
  void SetWasPlayedWithUserActivation(bool) override {}
  void OnRequestPictureInPicture() override {}

  WebTimeRanges Buffered() const override { return LoadedRange(); }
  WebTimeRanges Seekable() const override { return LoadedRange(); }

  void OnFrozen() override {}

  bool SetSinkId(const WebString&,
                 WebSetSinkIdCompleteCallback completion_callback) override {
    main_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(completion_callback),
                                  absl::optional<WebSetSinkIdError>()));
    return true;
  }

  // Without tracks, no frame or sample is ever expected.
  bool HasVideo() const override { return false; }
  bool HasAudio() const override { return false; }
  gfx::Size NaturalSize() const override { return gfx::Size(); }
  gfx::Size VisibleSize() const override { return gfx::Size(); }

  bool Paused() const override { return paused_; }
  bool Seeking() const override { return seeking_; }

  double Duration() const override {
    return ready_state_ == kReadyStateHaveNothing
               ? std::numeric_limits<double>::quiet_NaN()
               : kDurationSeconds;
  }

  double CurrentTime() const override {
    if (paused_ || ended_ || seeking_)
      return position_;
    const double elapsed = (base::TimeTicks::Now() - anchor_).InSecondsF();
    return std::clamp(position_ + elapsed * rate_, 0.0, kDurationSeconds);
  }

  bool IsEnded() const override { return ended_; }
  NetworkState GetNetworkState() const override { return network_state_; }
  ReadyState GetReadyState() const override { return ready_state_; }

  WebString GetErrorMessage() const override {
    return network_state_ == kNetworkStateFormatError
               ? WebString::FromUTF8(kUnsupportedSourceMessage)
               : WebString();
  }

  bool DidLoadingProgress() override {
    return std::exchange(loading_progressed_, false);
  }

  bool WouldTaintOrigin() const override { return false; }
  double MediaTimeForTimeValue(double time_value) const override {
    return time_value;
  }
  unsigned DecodedFrameCount() const override { return 0; }
  unsigned DroppedFrameCount() const override { return 0; }
  uint64_t AudioDecodedByteCount() const override { return 0; }
  uint64_t VideoDecodedByteCount() const override { return 0; }
  bool PassedTimingAllowOriginCheck() const override { return true; }
  void SetVolumeMultiplier(double) override {}
  void SuspendForFrameClosed() override {}
  bool HasAvailableVideoFrame() const override { return false; }
  void Paint(cc::PaintCanvas*, const gfx::Rect&, cc::PaintFlags&) override {}
  scoped_refptr<media::VideoFrame> GetCurrentFrameThenUpdate() override {
    return nullptr;
  }
  absl::optional<int> CurrentFrameId() const override { return absl::nullopt; }
  int GetDelegateId() override { return delegate_id_; }
  GURL GetSrcAfterRedirects() override { return url_; }
  base::WeakPtr<WebMediaPlayer> AsWeakPtr() override {
    return weak_factory_.GetWeakPtr();
  }

  // WebMediaPlayerDelegate::Observer
  void OnFrameHidden() override {}
  void OnFrameShown() override {}
  void OnIdleTimeout() override {}

 private:
  void PostTask(void (StubWebMediaPlayer::*task)()) {
    main_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(task, weak_factory_.GetWeakPtr()));
  }

  void FailLoad() { SetNetworkState(kNetworkStateFormatError); }

  void FinishLoad() {
    loading_progressed_ = true;
    SetReadyState(kReadyStateHaveMetadata);
    SetReadyState(kReadyStateHaveEnoughData);
    SetNetworkState(kNetworkStateLoaded);
  }

  void FinishSeek() {
    seeking_ = false;
    anchor_ = base::TimeTicks::Now();
    client_->TimeChanged();
    ScheduleEnd();
  }

  void End() {
    position_ = kDurationSeconds;
    ended_ = true;
    client_->TimeChanged();
  }

  // Schedules End() for when the position reaches the duration.
  void ScheduleEnd() {
    end_.Cancel();
    if (paused_ || ended_ || seeking_ || rate_ <= 0)
      return;
    end_.Reset(
        base::BindOnce(&StubWebMediaPlayer::End, base::Unretained(this)));
    main_task_runner_->PostDelayedTask(
        FROM_HERE, end_.callback(),
        base::Seconds((kDurationSeconds - CurrentTime()) / rate_));
  }

  // Folds the time played so far into |position_|.
  void Rebase() {
    position_ = CurrentTime();
    anchor_ = base::TimeTicks::Now();
  }

  WebTimeRanges LoadedRange() const {
    if (ready_state_ == kReadyStateHaveNothing)
      return WebTimeRanges();
    WebTimeRange range(0, kDurationSeconds);
    return WebTimeRanges(&range, 1);
  }

  void SetNetworkState(NetworkState state) {
    network_state_ = state;
    client_->NetworkStateChanged();
  }

  void SetReadyState(ReadyState state) {
    ready_state_ = state;
    client_->ReadyStateChanged();
  }

  WebMediaPlayerClient* const client_;
  WebMediaPlayerDelegate* const delegate_;
  const scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;
  const int delegate_id_;

  GURL url_;
  NetworkState network_state_ = kNetworkStateEmpty;
  ReadyState ready_state_ = kReadyStateHaveNothing;
  bool loading_progressed_ = false;

  // Playback position at |anchor_|, from which it advances at |rate_| while
  // playing.
  double position_ = 0;
  base::TimeTicks anchor_;
  double rate_ = 1;
  bool paused_ = true;
  bool seeking_ = false;
  bool ended_ = false;

  base::CancelableOnceClosure seek_done_;
  base::CancelableOnceClosure end_;
  base::WeakPtrFactory<StubWebMediaPlayer> weak_factory_{this};
};

}  // namespace

std::unique_ptr<WebMediaPlayer> CreateStubWebMediaPlayer(
    WebMediaPlayerClient* client,
    WebMediaPlayerDelegate* delegate,
    scoped_refptr<base::SingleThreadTaskRunner> main_task_runner) {
  return std::make_unique<StubWebMediaPlayer>(client, delegate,
                                              std::move(main_task_runner));
}

}  // namespace blink
