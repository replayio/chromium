// Copyright (c) 2021 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The V8 driver entry points //base calls, as two macro tables.
//
// These live in a header, rather than in base/record_replay.cc where they were
// written, so that base/record_replay_v8_stubs.cc can expand the same tables.
// The macros name no types themselves -- the types are resolved where the
// tables are expanded -- so this header needs no includes.
//
// Each entry in ForEachV8API carries a DefaultValue, which is the value the
// fork already returns on Windows when no driver is loaded.

#ifndef BASE_RECORD_REPLAY_V8_API_H_
#define BASE_RECORD_REPLAY_V8_API_H_

#define ForEachV8API(Macro)                                             \
  Macro(V8IsRecordingOrReplaying,                                       \
        (const char* feature, const char* subfeature),                  \
        (feature, subfeature), bool, false)                             \
  Macro(V8IsRecording, (), (), bool, false)                             \
  Macro(V8IsReplaying, (), (), bool, false)                             \
  Macro(V8GetRecordingId, (), (), char*, nullptr)                       \
  Macro(V8RecordReplayValue,                                            \
        (const char* why, uintptr_t value), (why, value), uintptr_t, value) \
  Macro(V8RecordReplayCreateOrderedLock,                                \
        (const char* name), (name), size_t, 0)                          \
  Macro(V8RecordReplayNewBookmark, (), (), uint64_t, 0)                 \
  Macro(V8RecordReplayAreEventsDisallowed,                              \
        (const char* why), (why), bool, false)                          \
  Macro(V8RecordReplayAreEventsPassedThrough,                           \
        (const char* why), (why), bool, false)                          \
  Macro(V8RecordReplayHasDivergedFromRecording, (), (), bool, false)    \
  Macro(V8RecordReplayUpdateDependencyGraph, (), (), bool, false)       \
  Macro(V8RecordReplayNewDependencyGraphNode,                           \
        (const char* json), (json), int, 0)                             \
  Macro(V8RecordReplayAllowSideEffects, (), (), bool, true)             \
  Macro(V8RecordReplayPointerId, (const void* ptr), (ptr), int, 0)      \
  Macro(V8RecordReplayIdPointer, (int id), (id), void*, nullptr)        \
  Macro(V8RecordReplayFeatureEnabled,                                   \
        (const char* feature, const char* subfeature),                  \
        (feature, subfeature), bool, true)                              \
  Macro(V8RecordReplayHasDisabledFeatures, (), (), bool, false)         \
  Macro(V8RecordReplayAreAssertsDisabled, (), (), bool, false)          \
  Macro(V8IsMainThread, (), (), bool, false)                            \
  Macro(V8RecordReplayIsInReplayCode,                                   \
        (const char* why), (why), bool, false)                          \
  Macro(V8RecordReplayHasAsserts, (), (), bool, false)                  \
  Macro(V8RecordReplayHadMismatch, (), (), bool, false)

#define ForEachV8APIVoid(Macro)                                         \
  Macro(V8RecordReplayAssertVA,                                         \
        (const char* format, va_list args),                             \
        (format, args))                                                 \
  Macro(V8RecordReplayAssertMaybeEventsDisallowedVA,                    \
        (const char* format, va_list args),                             \
        (format, args))                                                 \
  Macro(V8RecordReplayAssertBytes,                                      \
        (const char* why, const void* buf, size_t size),                \
        (why, buf, size))                                               \
  Macro(V8RecordReplayPrintVA,                                          \
        (const char* format, va_list args),                             \
        (format, args))                                                 \
  Macro(V8RecordReplayDiagnosticVA,                                     \
        (const char* format, va_list args),                             \
        (format, args))                                                 \
  Macro(V8RecordReplayCommandDiagnosticVA,                              \
        (const char* format, va_list args),                             \
        (format, args))                                                 \
  Macro(V8RecordReplayCommandDiagnosticTraceVA,                         \
        (const char* format, va_list args),                             \
        (format, args))                                                 \
  Macro(V8RecordReplayWarning,                                          \
        (const char* kind, const RecordReplayWarningField* fields,      \
         size_t count),                                                 \
        (kind, fields, count))                                          \
  Macro(V8RecordReplayTrace,                                            \
        (const char* format, va_list args),                             \
        (format, args))                                                 \
  Macro(V8RecordReplayCrash,                                            \
        (const char* format, va_list args),                             \
        (format, args))                                                 \
  Macro(V8RecordReplayBytes,                                            \
        (const char* why, void* buf, size_t size),                      \
        (why, buf, size))                                               \
  Macro(V8RecordReplayOrderedLock, (int lock), (lock))                  \
  Macro(V8RecordReplayOrderedUnlock, (int lock), (lock))                \
  Macro(V8RecordReplayNewCheckpoint, (), ())                            \
  Macro(V8RecordReplayNewCheckpointFlushed, (), ())                     \
  Macro(V8RecordReplaySetScriptAlive, (bool alive), (alive))            \
  Macro(V8RecordReplayOnAnnotation,                                     \
        (const char* kind, const char* contents),                       \
        (kind, contents))                                               \
  Macro(V8RecordReplayOnNetworkRequest,                                 \
    (const char* id, const char* kind, uint64_t bookmark),              \
    (id, kind, bookmark))                                               \
  Macro(V8RecordReplayOnNetworkRequestEvent, (const char* id), (id))    \
  Macro(V8RecordReplayOnNetworkStreamStart,                             \
        (const char* id, const char* kind, const char* parentId),       \
        (id, kind, parentId))                                           \
  Macro(V8RecordReplayOnNetworkStreamData,                              \
        (const char* id, size_t offset, size_t length, uint64_t bookmark), \
        (id, offset, length, bookmark))                                 \
  Macro(V8RecordReplayOnNetworkStreamEnd,                               \
        (const char* id, size_t length), (id, length))                  \
  Macro(V8RecordReplayBeginDisallowEvents, (), ())                      \
  Macro(V8RecordReplayBeginDisallowEventsWithLabel,                     \
        (const char* label), (label))                                   \
  Macro(V8RecordReplayEndDisallowEvents, (), ())                        \
  Macro(V8RecordReplayBeginPassThroughEvents, (), ())                   \
  Macro(V8RecordReplayEndPassThroughEvents, (), ())                     \
  Macro(V8RecordReplayRegisterPointer,                                  \
        (const char* name, const void* ptr), (name, ptr))               \
  Macro(V8RecordReplayUnregisterPointer, (const void* ptr), (ptr))      \
  Macro(V8RecordReplayBrowserEvent,                                     \
        (const char* name, const char* payload), (name, payload))       \
  Macro(V8RecordReplayOnEvent,                                          \
        (const char* event, bool before), (event, before))              \
  Macro(V8RecordReplayOnMouseEvent,                                     \
        (const char* kind, size_t clientX, size_t clientY, bool synthetic),\
        (kind, clientX, clientY, synthetic))                            \
  Macro(V8RecordReplayOnKeyEvent,                                       \
        (const char* kind, const char* key, bool synthetic),            \
        (kind, key, synthetic))                                         \
  Macro(V8RecordReplayOnNavigationEvent,                                \
        (const char* kind, const char* url), (kind, url))               \
  Macro(V8RecordReplayAddDependencyGraphEdge,                           \
        (int source, int target, const char* json), (source, target, json)) \
  Macro(V8RecordReplayBeginDependencyExecution, (int node), (node))     \
  Macro(V8RecordReplayEndDependencyExecution, (), ())                   \
  Macro(V8RecordReplayAddOrderedSRWLock,                                \
        (const char* name, void* lock), (name, lock))                   \
  Macro(V8RecordReplayRemoveOrderedSRWLock, (void* lock), (lock))       \
  Macro(V8RecordReplayMaybeTerminate,                                   \
        (void (*callback)(void*), void* data), (callback, data))        \
  Macro(V8RecordReplayFinishRecording, (), ())                          \
  Macro(V8RecordReplayGetCurrentJSStack,                                \
        (std::string* stackTrace), (stackTrace))                        \
  Macro(V8RecordReplayEnterReplayCode, (), ())                          \
  Macro(V8RecordReplayExitReplayCode, (), ())                           \
  Macro(V8RecordReplayBeginAssertBufferAllocations,                     \
    (const char* issueLabel), (issueLabel))                             \
  Macro(V8RecordReplayEndAssertBufferAllocations, (), ())

#endif  // BASE_RECORD_REPLAY_V8_API_H_
