// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/bindings/record_replay_gc_release.h"

#include "base/numerics/safe_conversions.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"

namespace blink {

namespace {

constexpr V8PerIsolateData::UserData::Key kKey =
    V8PerIsolateData::UserData::Key::kRecordReplayGCRelease;

}  // namespace

RecordReplayGCRelease::RecordReplayGCRelease(v8::Isolate* isolate)
    : isolate_(isolate) {}

// static
RecordReplayGCRelease* RecordReplayGCRelease::From(v8::Isolate* isolate) {
  if (!isolate) {
    return nullptr;
  }
  V8PerIsolateData* data = V8PerIsolateData::From(isolate);
  return data ? static_cast<RecordReplayGCRelease*>(data->GetUserData(kKey))
              : nullptr;
}

// static
int RecordReplayGCRelease::TrackRelease(v8::Isolate* isolate,
                                        base::OnceClosure release,
                                        base::OnceClosure keep_alive) {
  if (!isolate || recordreplay::AreEventsDisallowed()) {
    return 0;
  }
  V8PerIsolateData* data = V8PerIsolateData::From(isolate);
  if (!data) {
    return 0;
  }
  auto* releases = static_cast<RecordReplayGCRelease*>(data->GetUserData(kKey));
  if (!releases) {
    releases = MakeGarbageCollected<RecordReplayGCRelease>(isolate);
    data->SetUserData(kKey, releases);
    v8::replayio::AddReplayGCPollParticipant(isolate, releases);
  }
  int id = releases->next_id_++;
  releases->releases_.insert(
      id, Release{std::move(release), std::move(keep_alive)});
  return id;
}

// static
bool RecordReplayGCRelease::OnCollected(int id) {
  if (!id) {
    return false;
  }
  if (recordreplay::IsRecording()) {
    if (RecordReplayGCRelease* releases = From(v8::Isolate::TryGetCurrent())) {
      releases->collected_.push_back(id);
    }
  }
  return true;
}

// static
void RecordReplayGCRelease::ForcePoll(v8::Isolate* isolate) {
  // Clearing kept objects ends with a GC poll. It is only allowed between
  // script jobs, so a task nested in script (e.g. one a paused debugger runs)
  // does not poll here.
  if (From(isolate) && !isolate->InContext()) {
    isolate->ClearKeptObjects();
  }
}

void RecordReplayGCRelease::WillBeDestroyed() {
  v8::replayio::RemoveReplayGCPollParticipant(isolate_, this);
  releases_.clear();
  collected_.clear();
}

void RecordReplayGCRelease::Poll() {
  Vector<int> ids;
  ids.swap(collected_);
  ids.resize(base::checked_cast<wtf_size_t>(
      recordreplay::RecordReplayValue("GC releases", ids.size())));
  recordreplay::RecordReplayBytes("GC releases", ids.data(),
                                  ids.size() * sizeof(int));

  for (int id : ids) {
    auto it = releases_.find(id);
    if (it == releases_.end()) {
      continue;
    }
    Release release = std::move(it->value);
    releases_.erase(it);
    std::move(release.release).Run();
  }
}

}  // namespace blink
