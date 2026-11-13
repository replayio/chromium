// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/public/platform/media/stub_web_media_player.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/cancelable_callback.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "media/base/audio_decoder_config.h"
#include "media/base/demuxer.h"
#include "media/base/media_util.h"
#include "media/base/pipeline_status.h"
#include "media/base/ranges.h"
#include "media/base/video_decoder_config.h"
#include "media/filters/chunk_demuxer.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/public/platform/media/webmediaplayer_delegate.h"
#include "third_party/blink/public/platform/web_media_player_client.h"
#include "third_party/blink/public/platform/web_media_player_source.h"
#include "third_party/blink/public/platform/web_media_source.h"
#include "third_party/blink/public/platform/web_source_buffer.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_time_range.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/renderer/platform/media/web_media_source_impl.h"
#include "url/gurl.h"

namespace blink {

namespace {

// Every URL resource is this long.
constexpr double kUrlDurationSeconds = 10;

// Buffered data that starts this close to 0 starts the media, as in
// media::SourceBufferStream.
constexpr double kStartFudgeSeconds = 1;

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// A WebMediaSourceImpl that reports whether the stream is marked ended.
class StubWebMediaSource : public WebMediaSource {
 public:
  StubWebMediaSource(media::ChunkDemuxer* demuxer,
                     base::RepeatingCallback<void(bool)> on_end_of_stream)
      : media_source_(demuxer),
        on_end_of_stream_(std::move(on_end_of_stream)) {}

  std::unique_ptr<WebSourceBuffer> AddSourceBuffer(
      const WebString& content_type,
      const WebString& codecs,
      AddStatus& out_status) override {
    return media_source_.AddSourceBuffer(content_type, codecs, out_status);
  }
  std::unique_ptr<WebSourceBuffer> AddSourceBuffer(
      std::unique_ptr<media::AudioDecoderConfig> audio_config,
      AddStatus& out_status) override {
    return media_source_.AddSourceBuffer(std::move(audio_config), out_status);
  }
  std::unique_ptr<WebSourceBuffer> AddSourceBuffer(
      std::unique_ptr<media::VideoDecoderConfig> video_config,
      AddStatus& out_status) override {
    return media_source_.AddSourceBuffer(std::move(video_config), out_status);
  }

  double Duration() override { return media_source_.Duration(); }
  void SetDuration(double duration) override {
    media_source_.SetDuration(duration);
  }

  void MarkEndOfStream(EndOfStreamStatus status) override {
    media_source_.MarkEndOfStream(status);
    on_end_of_stream_.Run(status == kEndOfStreamStatusNoError);
  }

  void UnmarkEndOfStream() override {
    media_source_.UnmarkEndOfStream();
    on_end_of_stream_.Run(false);
  }

 private:
  WebMediaSourceImpl media_source_;
  const base::RepeatingCallback<void(bool)> on_end_of_stream_;
};

// Plays the buffered ranges of the resource on the clock. URL resources are
// fully buffered; Media Source buffers what the page appends, parsed by a
// media::ChunkDemuxer; MediaStreams are live.
class StubWebMediaPlayer : public WebMediaPlayer,
                           public WebMediaPlayerDelegate::Observer,
                           public media::DemuxerHost {
 public:
  StubWebMediaPlayer(
      WebMediaPlayerClient* client,
      WebMediaPlayerDelegate* delegate,
      scoped_refptr<base::SingleThreadTaskRunner> main_task_runner)
      : client_(client),
        delegate_(delegate),
        main_task_runner_(std::move(main_task_runner)),
        // An id of its own keeps the browser's player ids unique.
        delegate_id_(delegate_->AddObserver(this)) {
    weak_this_ = weak_factory_.GetWeakPtr();
  }

  ~StubWebMediaPlayer() override {
    if (demuxer_)
      demuxer_->Shutdown();
    delegate_->RemoveObserver(delegate_id_);
  }

  // WebMediaPlayer
  LoadTiming Load(LoadType load_type,
                  const WebMediaPlayerSource& source,
                  CorsMode,
                  bool is_cache_disabled) override {
    load_type_ = load_type;
    SetNetworkState(kNetworkStateLoading);
    if (load_type == kLoadTypeMediaSource) {
      StartMediaSource();
    } else {
      if (load_type == kLoadTypeURL)
        url_ = GURL(source.GetAsURL());
      PostTask(&StubWebMediaPlayer::FinishLoad);
    }
    return LoadTiming::kImmediate;
  }

  void Play() override {
    Rebase();
    paused_ = false;
    ScheduleHorizon();
  }

  void Pause() override {
    Rebase();
    paused_ = true;
    horizon_reached_.Cancel();
  }

  void Seek(double seconds) override {
    position_ = std::max(seconds, 0.0);
    const double duration = Duration();
    if (duration < position_)
      position_ = duration;
    ended_ = false;
    seeking_ = true;
    horizon_reached_.Cancel();
    PostTask(&StubWebMediaPlayer::MaybeFinishSeek);
  }

  void SetRate(double rate) override {
    Rebase();
    rate_ = rate;
    ScheduleHorizon();
  }

  void SetVolume(double) override {}
  void SetLatencyHint(double) override {}
  void SetPreservesPitch(bool) override {}
  void SetWasPlayedWithUserActivation(bool) override {}
  void OnRequestPictureInPicture() override {}

  WebTimeRanges Buffered() const override { return BufferedRanges(); }

  WebTimeRanges Seekable() const override {
    const double duration = Duration();
    if (load_type_ == kLoadTypeMediaStream || !std::isfinite(duration))
      return WebTimeRanges();
    WebTimeRange range(0, duration);
    return WebTimeRanges(&range, 1);
  }

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
    if (ready_state_ == kReadyStateHaveNothing)
      return std::numeric_limits<double>::quiet_NaN();
    if (load_type_ == kLoadTypeMediaSource)
      return demuxer_->GetDuration();
    if (load_type_ == kLoadTypeMediaStream)
      return kInfinity;
    return kUrlDurationSeconds;
  }

  double CurrentTime() const override {
    if (paused_ || ended_ || seeking_)
      return position_;
    const double elapsed = (base::TimeTicks::Now() - anchor_).InSecondsF();
    return std::min(position_ + elapsed * rate_, horizon_);
  }

  bool IsEnded() const override { return ended_; }
  NetworkState GetNetworkState() const override { return network_state_; }
  ReadyState GetReadyState() const override { return ready_state_; }

  WebString GetErrorMessage() const override {
    return error_ ? WebString::FromUTF8(media::PipelineStatusToString(*error_))
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

  // media::DemuxerHost. The demuxer calls these with its lock held, from the
  // thread that appends, which is a worker for MediaSource in workers.
  void OnBufferedTimeRangesChanged(
      const media::Ranges<base::TimeDelta>&) override {
    PostTask(&StubWebMediaPlayer::Refresh);
  }
  void SetDuration(base::TimeDelta) override {
    PostTask(&StubWebMediaPlayer::OnDurationChanged);
  }
  void OnDemuxerError(media::PipelineStatus status) override {
    main_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&StubWebMediaPlayer::Fail, weak_this_, status.code()));
  }

 private:
  void PostTask(void (StubWebMediaPlayer::*task)()) {
    main_task_runner_->PostTask(FROM_HERE, base::BindOnce(task, weak_this_));
  }

  void StartMediaSource() {
    demuxer_ = std::make_unique<media::ChunkDemuxer>(
        base::BindPostTask(
            main_task_runner_,
            base::BindOnce(&StubWebMediaPlayer::OpenMediaSource, weak_this_)),
        base::BindPostTask(
            main_task_runner_,
            base::BindRepeating(&StubWebMediaPlayer::OnProgress, weak_this_)),
        base::DoNothing(), &media_log_);
    demuxer_->Initialize(
        this, base::BindPostTask(
                  main_task_runner_,
                  base::BindOnce(&StubWebMediaPlayer::OnMediaSourceInitialized,
                                 weak_this_)));
  }

  void OpenMediaSource() {
    client_->MediaSourceOpened(new StubWebMediaSource(
        demuxer_.get(),
        base::BindPostTask(
            main_task_runner_,
            base::BindRepeating(&StubWebMediaPlayer::SetEndOfStream,
                                weak_this_))));
  }

  void OnMediaSourceInitialized(media::PipelineStatus status) {
    if (!status.is_ok()) {
      Fail(status.code());
      return;
    }
    OnMetadata();
  }

  void OnProgress() { loading_progressed_ = true; }

  void SetEndOfStream(bool end_of_stream) {
    end_of_stream_ = end_of_stream;
    Refresh();
  }

  void OnDurationChanged() {
    if (ready_state_ == kReadyStateHaveNothing)
      return;
    client_->DurationChanged();
    Refresh();
  }

  void FinishLoad() {
    loading_progressed_ = true;
    OnMetadata();
    if (load_type_ == kLoadTypeURL)
      SetNetworkState(kNetworkStateLoaded);
  }

  void OnMetadata() {
    SetReadyState(kReadyStateHaveMetadata);
    Refresh();
  }

  void Fail(media::PipelineStatusCodes code) {
    if (error_)
      return;
    error_ = code;
    horizon_reached_.Cancel();
    if (ready_state_ == kReadyStateHaveNothing)
      SetNetworkState(kNetworkStateFormatError);
    else if (code == media::CHUNK_DEMUXER_ERROR_EOS_STATUS_NETWORK_ERROR)
      SetNetworkState(kNetworkStateNetworkError);
    else
      SetNetworkState(kNetworkStateDecodeError);
  }

  // Re-evaluates playback after the buffered ranges, the duration or the end
  // of stream changed.
  void Refresh() {
    if (ready_state_ == kReadyStateHaveNothing || error_)
      return;
    Rebase();
    horizon_ = Horizon(position_);
    if (seeking_) {
      MaybeFinishSeek();
      return;
    }
    if (ended_)
      return;
    UpdateReadyState();
    ScheduleHorizon();
  }

  void MaybeFinishSeek() {
    if (!seeking_ || error_)
      return;
    horizon_ = Horizon(position_);
    if (!CanPlayAt(position_)) {
      if (ready_state_ > kReadyStateHaveMetadata)
        SetReadyState(kReadyStateHaveMetadata);
      return;
    }
    seeking_ = false;
    anchor_ = base::TimeTicks::Now();
    UpdateReadyState();
    client_->TimeChanged();
    ScheduleHorizon();
  }

  // Schedules OnHorizonReached() for when the position reaches |horizon_|.
  void ScheduleHorizon() {
    horizon_reached_.Cancel();
    if (ready_state_ == kReadyStateHaveNothing || paused_ || ended_ ||
        seeking_ || error_ || rate_ <= 0 || !std::isfinite(horizon_)) {
      return;
    }
    const double now = CurrentTime();
    if (!CanPlayAt(now))
      return;
    horizon_reached_.Reset(base::BindOnce(
        &StubWebMediaPlayer::OnHorizonReached, base::Unretained(this)));
    main_task_runner_->PostDelayedTask(
        FROM_HERE, horizon_reached_.callback(),
        base::Seconds((horizon_ - now) / rate_));
  }

  void OnHorizonReached() {
    position_ = horizon_;
    anchor_ = base::TimeTicks::Now();
    if (IsComplete() && position_ >= Duration()) {
      ended_ = true;
      client_->TimeChanged();
      return;
    }
    horizon_ = Horizon(position_);
    ScheduleHorizon();
    UpdateReadyState();
  }

  void UpdateReadyState() {
    ReadyState state = kReadyStateHaveEnoughData;
    if (!CanPlayAt(position_)) {
      state = ready_state_ >= kReadyStateHaveCurrentData
                  ? kReadyStateHaveCurrentData
                  : kReadyStateHaveMetadata;
    }
    if (state != ready_state_)
      SetReadyState(state);
  }

  // Folds the time played so far into |position_|.
  void Rebase() {
    position_ = CurrentTime();
    anchor_ = base::TimeTicks::Now();
  }

  // Whether no data follows the buffered ranges.
  bool IsComplete() const {
    if (load_type_ == kLoadTypeMediaSource)
      return end_of_stream_;
    return load_type_ == kLoadTypeURL;
  }

  bool CanPlayAt(double position) const {
    return Horizon(position) > position ||
           (IsComplete() && position >= Duration());
  }

  // How far playback from |position| can advance without more data.
  double Horizon(double position) const {
    if (load_type_ == kLoadTypeMediaStream)
      return kInfinity;
    const double duration = Duration();
    if (!(position < duration))
      return position;
    const WebTimeRanges ranges = BufferedRanges();
    for (size_t i = 0; i < ranges.size(); ++i) {
      const double start =
          i == 0 && ranges[i].start < kStartFudgeSeconds ? 0 : ranges[i].start;
      if (start <= position && position < ranges[i].end)
        return std::min(ranges[i].end, duration);
    }
    return position;
  }

  WebTimeRanges BufferedRanges() const {
    if (load_type_ == kLoadTypeMediaSource) {
      const media::Ranges<base::TimeDelta> ranges =
          demuxer_->GetBufferedRanges();
      WebTimeRanges result(ranges.size());
      for (size_t i = 0; i < ranges.size(); ++i) {
        result[i].start = ranges.start(i).InSecondsF();
        result[i].end = ranges.end(i).InSecondsF();
      }
      return result;
    }
    if (load_type_ == kLoadTypeMediaStream ||
        ready_state_ == kReadyStateHaveNothing) {
      return WebTimeRanges();
    }
    WebTimeRange range(0, kUrlDurationSeconds);
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

  LoadType load_type_ = kLoadTypeURL;
  GURL url_;
  NetworkState network_state_ = kNetworkStateEmpty;
  ReadyState ready_state_ = kReadyStateHaveNothing;
  bool loading_progressed_ = false;
  absl::optional<media::PipelineStatusCodes> error_;

  // Media Source only.
  media::NullMediaLog media_log_;
  std::unique_ptr<media::ChunkDemuxer> demuxer_;
  bool end_of_stream_ = false;

  // Playback position at |anchor_|, from which it advances at |rate_| while
  // playing, up to |horizon_|.
  double position_ = 0;
  base::TimeTicks anchor_;
  double horizon_ = 0;
  double rate_ = 1;
  bool paused_ = true;
  bool seeking_ = false;
  bool ended_ = false;

  base::CancelableOnceClosure horizon_reached_;
  // Bound on the main thread; copied by tasks posted from other threads.
  base::WeakPtr<StubWebMediaPlayer> weak_this_;
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
