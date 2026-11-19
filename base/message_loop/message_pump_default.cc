// Copyright 2006-2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/message_loop/message_pump_default.h"

#include <stdlib.h>

#include "base/auto_reset.h"
#include "base/logging.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_APPLE)
#include <mach/thread_policy.h>

#include "base/mac/mach_logging.h"
#include "base/mac/scoped_mach_port.h"
#include "base/mac/scoped_nsautorelease_pool.h"
#include "base/threading/threading_features.h"
#endif

#include "base/record_replay.h"

namespace base {

namespace {

// How long the main thread idles before what it recorded is flushed, see
// MessagePumpDefault::Run.
constexpr TimeDelta kRecordReplayIdleFlushDelay = Milliseconds(500);

// Whether the recording is uploaded while it is made. Main thread only.
bool RecordReplayStreamedUpload() {
  static const bool streamed = [] {
    const char* env = getenv("RECORD_REPLAY_STREAMED_UPLOAD");
    return !!recordreplay::RecordReplayValue(
        "StreamedUpload", env && env[0] && env[0] != '0');
  }();
  return streamed;
}

}  // namespace

MessagePumpDefault::MessagePumpDefault()
    : keep_running_(true),
      event_(WaitableEvent::ResetPolicy::AUTOMATIC,
             WaitableEvent::InitialState::NOT_SIGNALED) {
  event_.declare_only_used_while_idle();
}

MessagePumpDefault::~MessagePumpDefault() = default;

void MessagePumpDefault::Run(Delegate* delegate) {
  AutoReset<bool> auto_reset_keep_running(&keep_running_, true);

  // Whether a flushed checkpoint was created since the thread last waited
  // for work.
  bool record_replay_idle_flushed = false;

  for (;;) {
#if BUILDFLAG(IS_APPLE)
    mac::ScopedNSAutoreleasePool autorelease_pool;
#endif

#if BUILDFLAG(IS_WIN)
    recordreplay::Assert("[RUN-2214] MessagePumpDefault::Run #1");
#endif

    Delegate::NextWorkInfo next_work_info = delegate->DoWork();
    bool has_more_immediate_work = next_work_info.is_immediate();

#if BUILDFLAG(IS_WIN)
    recordreplay::Assert("[RUN-2214] MessagePumpDefault::Run #2");
#endif

    if (!keep_running_) {
#if BUILDFLAG(IS_WIN)
      recordreplay::Assert("[RUN-2214] MessagePumpDefault::Run #3");
#endif
      break;
    }

    if (has_more_immediate_work) {
#if BUILDFLAG(IS_WIN)
      recordreplay::Assert("[RUN-2214] MessagePumpDefault::Run #4");
#endif
      continue;
    }

    has_more_immediate_work = delegate->DoIdleWork();
    if (!keep_running_) {
#if BUILDFLAG(IS_WIN)
      recordreplay::Assert("[RUN-2214] MessagePumpDefault::Run #5");
#endif
      break;
    }

    if (has_more_immediate_work) {
#if BUILDFLAG(IS_WIN)
      recordreplay::Assert("[RUN-2214] MessagePumpDefault::Run #6");
#endif
      continue;
    }

#if BUILDFLAG(IS_WIN)
    recordreplay::Assert("[RUN-2214] MessagePumpDefault::Run #7");
#endif

    // A recording that is uploaded while it is made is available up to its
    // last flushed checkpoint. Checkpoints are created as tasks run, so what
    // the main thread recorded before it went idle would stay unavailable
    // until its next task. When it has no work for a while, a flushed
    // checkpoint is created before it waits for good.
    if (!record_replay_idle_flushed && recordreplay::IsRecordingOrReplaying() &&
        recordreplay::IsMainThread() && RecordReplayStreamedUpload() &&
        (next_work_info.delayed_run_time.is_max() ||
         next_work_info.remaining_delay() > kRecordReplayIdleFlushDelay)) {
      bool signaled = event_.TimedWait(kRecordReplayIdleFlushDelay);
      if (recordreplay::RecordReplayValue("MessagePumpIdleFlush", !signaled)) {
        recordreplay::NewCheckpointFlushed();
        record_replay_idle_flushed = true;
      }
      continue;
    }
    record_replay_idle_flushed = false;

    if (next_work_info.delayed_run_time.is_max()) {
      event_.Wait();
    } else {
      event_.TimedWait(next_work_info.remaining_delay());
    }
    // Since event_ is auto-reset, we don't need to do anything special here
    // other than service each delegate method.
  }
}

void MessagePumpDefault::Quit() {
  keep_running_ = false;
}

void MessagePumpDefault::ScheduleWork() {
  // Since this can be called on any thread, we need to ensure that our Run
  // loop wakes up.
  event_.Signal();
}

void MessagePumpDefault::ScheduleDelayedWork(
    const Delegate::NextWorkInfo& next_work_info) {
  // Since this is always called from the same thread as Run(), there is nothing
  // to do as the loop is already running. It will wait in Run() with the
  // correct timeout when it's out of immediate tasks.
  // TODO(gab): Consider removing ScheduleDelayedWork() when all pumps function
  // this way (bit.ly/merge-message-pump-do-work).
}

#if BUILDFLAG(IS_APPLE)
void MessagePumpDefault::SetTimerSlack(TimerSlack timer_slack) {
  if (!FeatureList::GetInstance() ||
      !FeatureList::IsEnabled(kUseThreadQoSMac)) {
    thread_latency_qos_policy_data_t policy{};
    policy.thread_latency_qos_tier = timer_slack == TIMER_SLACK_MAXIMUM
                                         ? LATENCY_QOS_TIER_3
                                         : LATENCY_QOS_TIER_UNSPECIFIED;
    mac::ScopedMachSendRight thread_port(mach_thread_self());
    kern_return_t kr =
        thread_policy_set(thread_port.get(), THREAD_LATENCY_QOS_POLICY,
                          reinterpret_cast<thread_policy_t>(&policy),
                          THREAD_LATENCY_QOS_POLICY_COUNT);
    MACH_DVLOG_IF(1, kr != KERN_SUCCESS, kr) << "thread_policy_set";
  }
}
#endif

}  // namespace base
