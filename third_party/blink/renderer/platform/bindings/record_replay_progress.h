// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_RECORD_REPLAY_PROGRESS_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_RECORD_REPLAY_PROGRESS_H_

namespace blink {

// Implemented in v8/src/api/api.cc. Advances the record/replay execution
// progress counter, gated by the same condition the bytecode emitter uses.
extern "C" void V8RecordReplayAdvanceProgressCounter();

// Called at the entry of every generated V8 binding callback (method, attribute
// getter/setter, constructor) so that JS->C++ binding calls advance progress,
// which JS bytecode opcodes alone do not capture. All gating lives in V8.
inline void RecordReplayOnBindingProgress() {
  V8RecordReplayAdvanceProgressCounter();
}

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_BINDINGS_RECORD_REPLAY_PROGRESS_H_
