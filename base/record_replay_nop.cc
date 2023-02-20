// Copyright (c) 2021 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Define implementations for extern functions referenced by record_replay.cc
// which are normally implemented by V8. This can be used to link executables
// which include record_replay.cc but not V8 itself.

#include "base/record_replay.h"

extern "C" bool V8IsRecordingOrReplaying(const char* feature) { return false; }
extern "C" bool V8IsRecording() { return false; }
extern "C" bool V8IsReplaying() { return false; }
extern "C" char* V8GetRecordingId() { return nullptr; }
extern "C" void V8RecordReplayAssertVA(const char* format, va_list args) {}
extern "C" void V8RecordReplayAssertBytes(const char* why, const void* buf, size_t size) {}
extern "C" void V8RecordReplayPrintVA(const char* format, va_list args) {}
extern "C" void V8RecordReplayDiagnosticVA(const char* format, va_list args) {}
extern "C" uintptr_t V8RecordReplayValue(const char* why, uintptr_t value) { return value; }
extern "C" void V8RecordReplayBytes(const char* why, void* buf, size_t size) {}
extern "C" size_t V8RecordReplayCreateOrderedLock(const char* name) { return 0; }
extern "C" void V8RecordReplayOrderedLock(int lock) {}
extern "C" void V8RecordReplayOrderedUnlock(int lock) {}
extern "C" void V8RecordReplayNewCheckpoint() {}
extern "C" uint64_t V8RecordReplayNewBookmark() { return 0; }
extern "C" void V8RecordReplayOnAnnotation(const char* kind, const char* contents) {}
extern "C" void V8RecordReplayOnNetworkRequest(const char* id, const char* kind, uint64_t bookmark) {}
extern "C" void V8RecordReplayOnNetworkRequestEvent(const char* id) {}
extern "C" void V8RecordReplayOnNetworkStreamStart(const char* id, const char* kind, const char* parentId) {}
extern "C" void V8RecordReplayOnNetworkStreamData(const char* id, size_t offset, size_t length, uint64_t bookmark) {}
extern "C" void V8RecordReplayOnNetworkStreamEnd(const char* id, size_t length) {}
extern "C" bool V8RecordReplayAreEventsDisallowed() { return false; }
extern "C" void V8RecordReplayBeginDisallowEvents() {}
extern "C" void V8RecordReplayBeginDisallowEventsWithLabel(const char* label) {}
extern "C" void V8RecordReplayEndDisallowEvents() {}
extern "C" bool V8RecordReplayAreEventsPassedThrough() { return false; }
extern "C" void V8RecordReplayBeginPassThroughEvents() {}
extern "C" void V8RecordReplayEndPassThroughEvents() {}
extern "C" bool V8RecordReplayHasDivergedFromRecording() { return false; }
extern "C" void V8RecordReplayRegisterPointer(const char* name, const void* ptr) {}
extern "C" void V8RecordReplayUnregisterPointer(const void* ptr) {}
extern "C" int V8RecordReplayPointerId(const void* ptr) { return 0; }
extern "C" void* V8RecordReplayIdPointer(int id) { return nullptr; }
extern "C" bool V8RecordReplayFeatureEnabled(const char* feature) { return false; }
extern "C" void V8RecordReplayBrowserEvent(const char* name, const char* payload) {}
extern "C" bool V8IsMainThread() { return false; }
extern "C" void V8RecordReplayOnEvent(const char* aEvent, bool aBefore) {}
extern "C" void V8RecordReplayOnMouseEvent(const char* kind,
                                           size_t clientX,
                                           size_t clientY) {}
extern "C" void V8RecordReplayOnKeyEvent(const char* kind, const char* key) {}
extern "C" void V8RecordReplayOnNavigationEvent(const char* kind,
                                                const char* url) {}

#if BUILDFLAG(IS_WIN)
extern "C" void V8RecordReplayAddOrderedSRWLock(const char* name, void* aLock) {}
#endif
