// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_RECORD_REPLAY_GC_RELEASE_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_RECORD_REPLAY_GC_RELEASE_H_

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "v8/include/replayio.h"

namespace blink {

// Runs what the GC does when it collects an object at a point which replays,
// when recording or replaying.
//
// Finalizers and weak callbacks run at whichever GC collects an object, and a
// replay's collections don't happen at the recording's points. An object whose
// collection has effects other code sees can use this instead, as
// FinalizationRegistries do (see v8/src/replay/finalization-registry.h):
//
// - Where the object is made, Track() keeps the release, a callback doing what
//   collecting the object would. It must not keep the object alive, but can
//   hold it weakly.
// - When the GC collects the object, its pre-finalizer or weak callback calls
//   OnCollected(), which only notes the collection when recording.
// - At the isolate's next GC poll, the release runs on both sides for every
//   object the recording's GC collected. The replay keeps each object alive
//   until then, so a release which holds the object weakly finds it collected
//   when recording and alive when replaying.
//
// The GC poll runs at microtask checkpoints which had work to do. Code which
// reads state the GC changes can call ForcePoll() first.
class PLATFORM_EXPORT RecordReplayGCRelease final
    : public V8PerIsolateData::UserData,
      public v8::replayio::ReplayGCPollParticipant {
 public:
  // Returns the id to pass to OnCollected(), or 0 when the object is left to
  // the GC: when not recording or replaying, or at a point which does not
  // replay.
  template <typename T>
  static int Track(v8::Isolate* isolate,
                   T* object,
                   base::OnceClosure release) {
    if (!recordreplay::IsRecordingOrReplaying()) {
      return 0;
    }
    base::OnceClosure keep_alive;
    if (recordreplay::IsReplaying()) {
      keep_alive = BindOnce([](T*) {}, WrapPersistent(object));
    }
    return TrackRelease(isolate, std::move(release), std::move(keep_alive));
  }

  // Returns whether the release runs at the GC poll instead.
  static bool OnCollected(int id);

  static void ForcePoll(v8::Isolate* isolate);

  explicit RecordReplayGCRelease(v8::Isolate* isolate);

  // V8PerIsolateData::UserData:
  void WillBeDestroyed() override;

  // v8::replayio::ReplayGCPollParticipant:
  bool IsActive() const override { return !releases_.empty(); }
  bool HasPending() const override { return !collected_.empty(); }
  void Poll() override;

 private:
  struct Release {
    base::OnceClosure release;
    // When replaying: keeps the object alive.
    base::OnceClosure keep_alive;
  };

  static RecordReplayGCRelease* From(v8::Isolate* isolate);
  static int TrackRelease(v8::Isolate* isolate,
                          base::OnceClosure release,
                          base::OnceClosure keep_alive);

  const raw_ptr<v8::Isolate> isolate_;
  int next_id_ = 1;
  HashMap<int, Release> releases_;
  // When recording: the objects the GC collected since the last poll.
  Vector<int> collected_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_RECORD_REPLAY_GC_RELEASE_H_
