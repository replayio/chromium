// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/synchronization/waitable_event.h"

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/record_replay.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/threading/thread_restrictions.h"
#include "base/trace_event/trace_event.h"
#include "base/tracing_buildflags.h"
#include "build/build_config.h"

namespace base {

WaitableEvent::~WaitableEvent() {
  recordreplay::UnregisterPointer(this);
  // As requested in the documentation of perfetto::Flow::FromPointer, we should
  // emit a TerminatingFlow(this) from our destructor if we ever emitted a
  // Flow(this) which may be unmatched since the ptr value of `this` may be
  // reused after this destructor. This can happen if a signaled event is never
  // waited upon (or isn't the one to satisfy a WaitMany condition).
  if (!only_used_while_idle_) {
    // Check the tracing state to avoid an unnecessary syscall on destruction
    // (which can be performance sensitive, crbug.com/40275035).
    if (TRACE_EVENT_CATEGORY_ENABLED("wakeup.flow,toplevel.flow") &&
        IsSignaled()) {
      TRACE_EVENT_INSTANT("wakeup.flow,toplevel.flow",
                          "~WaitableEvent while Signaled",
                          perfetto::TerminatingFlow::FromPointer(this));
    }
  }
}

void WaitableEvent::Signal() {
#if BUILDFLAG(IS_APPLE)
  // Only the Mach port implementation has an ordered lock per event; the
  // POSIX one goes by WaitableEventKernel::record_replay_unordered_.
  std::optional<recordreplay::AutoDisallowEvents> disallow;
  if (!record_replay_ordered_lock_id_)
    disallow.emplace("WaitableEvent::Signal");
#endif

  // Must be ordered before SignalImpl() to guarantee it's emitted before the
  // matching TerminatingFlow in TimedWait().
  if (!only_used_while_idle_) {
    TRACE_EVENT_INSTANT("wakeup.flow,toplevel.flow", "WaitableEvent::Signal",
                        perfetto::Flow::FromPointer(this));
  }
  SignalImpl();
}

void WaitableEvent::Wait(const Location& location) {
  const bool result = TimedWait(TimeDelta::Max(), location);
  DCHECK(result) << "TimedWait() should never fail with infinite timeout";
}

bool WaitableEvent::TimedWait(TimeDelta wait_delta, const Location& location) {
  if (wait_delta <= TimeDelta()) {
    return IsSignaled();
  }

#if BUILDFLAG(IS_APPLE)
  // See Signal().
  std::optional<recordreplay::AutoDisallowEvents> disallow;
  if (!record_replay_ordered_lock_id_)
    disallow.emplace("WaitableEvent::TimedWait");
#endif

  // Consider this thread blocked unless the event is already signaled. Ignore
  // this for non-blocking WaitableEvents.
  std::optional<internal::ScopedBlockingCallWithBaseSyncPrimitives>
      scoped_blocking_call;

  if (!only_used_while_idle_) {
    // Always verify thread restrictions to avoid fortuitous allowance if it's
    // already signaled.
    internal::AssertBaseSyncPrimitivesAllowed();
    if (IsDefinitelySignaled()) {
      return true;
    }
    scoped_blocking_call.emplace(location, BlockingType::WILL_BLOCK);
  }

  const bool result = TimedWaitImpl(wait_delta);

  if (result && !only_used_while_idle_) {
    TRACE_EVENT_INSTANT("wakeup.flow,toplevel.flow",
                        "WaitableEvent::Wait Complete",
                        perfetto::TerminatingFlow::FromPointer(this));
  }

  return result;
}

size_t WaitableEvent::WaitMany(base::span<WaitableEvent*> events) {
  DCHECK(!events.empty()) << "Cannot wait on no events";

  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i]->IsDefinitelySignaled()) {
      return i;
    }
  }

  internal::ScopedBlockingCallWithBaseSyncPrimitives scoped_blocking_call(
      FROM_HERE, BlockingType::WILL_BLOCK);

  const size_t signaled_id = WaitManyImpl(events);
  WaitableEvent* const signaled_event = events[signaled_id];
  if (!signaled_event->only_used_while_idle_) {
    TRACE_EVENT_INSTANT("wakeup.flow,toplevel.flow",
                        "WaitableEvent::WaitMany Complete",
                        perfetto::TerminatingFlow::FromPointer(signaled_event));
  }
  return signaled_id;
}

OnceClosure WaitableEvent::GetWaitCallbackForTesting() {
  return BindOnce(&WaitableEvent::Wait, Unretained(this), FROM_HERE);
}

}  // namespace base
