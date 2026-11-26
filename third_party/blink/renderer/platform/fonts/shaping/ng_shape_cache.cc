// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/fonts/shaping/ng_shape_cache.h"

#include "third_party/blink/renderer/platform/bindings/record_replay_gc_release.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"

namespace blink {

bool NGShapeCache::RecordReplayPrepareLookup(const ShapeCacheKey& key) {
  // A pause's layout neither reads nor fills the cache, which would make the
  // replay's entries differ from the recording's.
  if (recordreplay::AreEventsDisallowed()) {
    return false;
  }
  v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
  if (!isolate) {
    return false;
  }
  if (record_replay_entries_.Contains(key)) {
    RecordReplayGCRelease::ForcePoll(isolate);
  }
  return true;
}

void NGShapeCache::RecordReplayInsert(const ShapeCacheKey& key,
                                      const ShapeResult* shape_result) {
  if (record_replay_entries_.size() >= kMaxSize) {
    return;
  }
  uint64_t token = record_replay_next_token_++;
  int id = RecordReplayGCRelease::Track(
      v8::Isolate::GetCurrent(), shape_result,
      blink::BindOnce(&NGShapeCache::RecordReplayRelease,
                      WrapWeakPersistent(this), key, token));
  if (!id) {
    return;
  }
  weak_map_.Set(key, shape_result);
  record_replay_entries_.Set(key, RecordReplayEntry{token, id, shape_result});
}

void NGShapeCache::RecordReplayRelease(const ShapeCacheKey& key,
                                       uint64_t token) {
  auto it = record_replay_entries_.find(key);
  if (it == record_replay_entries_.end() || it->value.token != token) {
    return;
  }
  record_replay_entries_.erase(it);
  // When recording, the GC has already cleared the entry.
  weak_map_.erase(key);
}

void NGShapeCache::RecordReplayTrace(Visitor* visitor) const {
  if (recordreplay::IsRecording()) {
    visitor->RegisterWeakCallbackMethod<NGShapeCache,
                                        &NGShapeCache::RecordReplayProcessWeak>(
        this);
  }
}

void NGShapeCache::RecordReplayProcessWeak(const LivenessBroker& broker) {
  for (auto& entry : record_replay_entries_) {
    if (entry.value.result && !broker.IsHeapObjectAlive(entry.value.result)) {
      RecordReplayGCRelease::OnCollected(entry.value.id);
      entry.value.result = nullptr;
    }
  }
}

void NGShapeCache::RecordReplayDispose() {
  // The results this cache's GC collects along with it are not seen by its
  // weak callback, but still have to be released.
  if (recordreplay::IsRecording()) {
    for (auto& entry : record_replay_entries_) {
      if (entry.value.result) {
        RecordReplayGCRelease::OnCollected(entry.value.id);
      }
    }
  }
}

}  // namespace blink
