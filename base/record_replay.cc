// Copyright (c) 2021 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/record_replay.h"
#include "base/compiler_specific.h"
#include "base/record_replay_v8_api.h"
#include "base/json/json_writer.h"
#include "base/values.h"

#include <atomic>
#include <stdarg.h>
#include <sstream>

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace recordreplay {

  

#if BUILDFLAG(IS_WIN)

#define DefineFunction(Name, Formals, Args, ReturnType, DefaultValue) \
  static ReturnType (*g##Name) Formals;                               \
  static inline ReturnType Name Formals {                             \
    return g##Name ? g##Name Args : DefaultValue;                     \
  }
ForEachV8API(DefineFunction)
#undef DefineFunction

#define DefineFunctionVoid(Name, Formals, Args) \
  static void (*g##Name) Formals;               \
  static inline void Name Formals {             \
    if (g##Name)                                \
      g##Name Args;                             \
  }
ForEachV8APIVoid(DefineFunctionVoid)
#undef DefineFunctionVoid

static void InitializationError(const char* format, ...) {
  {
    va_list args;
    va_start(args, format);
    UNSAFE_TODO(vfprintf(stderr, format, args));
    va_end(args);
  }

#if BUILDFLAG(IS_WIN)
  // Additionally write the message to a new file. Capturing the output written to
  // stderr by browser subprocesses on windows is surprisingly difficult.
  const char* dir = getenv("RECORD_REPLAY_LOG_DIRECTORY");
  char file[1024];
  snprintf(file, sizeof(file), "%s\\record_replay_initialization_error.txt", dir ? dir : ".");
  FILE* f = fopen(file, "w");
  if (f) {
    va_list args;
    va_start(args, format);
    UNSAFE_TODO(vfprintf(f, format, args));
    va_end(args);
    fclose(f);
  }
#endif

  CHECK(0);
}

void InitBindings() {
  HMODULE module;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			  reinterpret_cast<LPCSTR>(InitBindings),
			  &module)) {
    InitializationError("GetModuleHandleExA failed %d, crashing...\n", (int)GetLastError());
  }

#define LoadFunction(Name, Formals, Args, ReturnType, DefaultValue)           \
  g##Name = reinterpret_cast<ReturnType(*)Formals>(GetProcAddress(module, #Name)); \
  if (!g##Name) {                                                             \
    InitializationError("Could not find V8 export %s, crashing...\n", #Name); \
  }
ForEachV8API(LoadFunction)
#undef LoadFunction

#define LoadFunctionVoid(Name, Formals, Args)                                 \
  g##Name = reinterpret_cast<void(*)Formals>(GetProcAddress(module, #Name)); \
  if (!g##Name) {                                                             \
    InitializationError("Could not find V8 export %s, crashing...\n", #Name); \
  }
ForEachV8APIVoid(LoadFunctionVoid)
#undef LoadFunctionVoid
}

#else // !BUILD_FLAG(IS_WIN)

#define DefineFunction(Name, Formals, Args, ReturnType, DefaultValue) \
  extern "C" ReturnType Name Formals;
ForEachV8API(DefineFunction)
#undef DefineFunction

#define DefineFunctionVoid(Name, Formals, Args) \
  extern "C" void Name Formals;
ForEachV8APIVoid(DefineFunctionVoid)
#undef DefineFunction

#endif // !BUILD_FLAG(IS_WIN)

bool IsRecordingOrReplaying(const char* feature, const char* subfeature) {
  return V8IsRecordingOrReplaying(feature, subfeature);
}

bool IsRecording() {
  return V8IsRecording();
}

bool IsReplaying() {
  return V8IsReplaying();
}

char* GetRecordingId() {
  return V8GetRecordingId();
}

bool HadMismatch() {
  return V8RecordReplayHadMismatch();
}

bool HasAsserts() {
  return V8RecordReplayHasAsserts();
}

void Assert(const char* format, ...) {
#ifndef NACL_TC_REV
  va_list ap;
  va_start(ap, format);
  V8RecordReplayAssertVA(format, ap);
  va_end(ap);
#endif
}

void AssertMaybeEventsDisallowed(const char* format, ...) {
#ifndef NACL_TC_REV
  va_list ap;
  va_start(ap, format);
  V8RecordReplayAssertMaybeEventsDisallowedVA(format, ap);
  va_end(ap);
#endif
}

void Diagnostic(const char* format, ...) {
#ifndef NACL_TC_REV
  va_list ap;
  va_start(ap, format);
  V8RecordReplayDiagnosticVA(format, ap);
  va_end(ap);
#endif
}

void CommandDiagnostic(const char* format, ...) {
#ifndef NACL_TC_REV
  va_list ap;
  va_start(ap, format);
  V8RecordReplayCommandDiagnosticVA(format, ap);
  va_end(ap);
#endif
}

void CommandDiagnosticTrace(const char* format, ...) {
#ifndef NACL_TC_REV
  va_list ap;
  va_start(ap, format);
  V8RecordReplayCommandDiagnosticTraceVA(format, ap);
  va_end(ap);
#endif
}

void Warning(const char* kind,
             std::initializer_list<RecordReplayWarningField> fields) {
  Warning(kind, fields.begin(), fields.size());
}

void Warning(const char* kind,
             const RecordReplayWarningField* fields,
             size_t count) {
#ifndef NACL_TC_REV
  V8RecordReplayWarning(kind, fields, count);
#endif
}

void Trace(const char* format, ...) {
#ifndef NACL_TC_REV
  va_list ap;
  va_start(ap, format);
  V8RecordReplayTrace(format, ap);
  va_end(ap);
#endif
}

void AssertBytes(const char* why, const void* buf, size_t size) {
  V8RecordReplayAssertBytes(why, buf, size);
}

bool AreAssertsDisabled() {
  return V8RecordReplayAreAssertsDisabled();
}

void Print(const char* format, ...) {
#ifndef NACL_TC_REV
  va_list ap;
  va_start(ap, format);
  V8RecordReplayPrintVA(format, ap);
  va_end(ap);
#endif
}

uintptr_t RecordReplayValue(const char* why, uintptr_t v) {
  return V8RecordReplayValue(why, v);
}

void RecordReplayBytes(const char* why, void* buf, size_t size) {
  V8RecordReplayBytes(why, buf, size);
}

void RecordReplayString(const char* why, std::string& str) {
  size_t length = RecordReplayValue(why, str.length());
  str.resize(length);
  if (length) {
    RecordReplayBytes(why, &str[0], length);
  }
}

int CreateOrderedLock(const char* name) {
  return (int)V8RecordReplayCreateOrderedLock(name);
}

void OrderedLock(int lock) {
  V8RecordReplayOrderedLock(lock);
}

void OrderedUnlock(int lock) {
  V8RecordReplayOrderedUnlock(lock);
}

void NewCheckpoint() {
  V8RecordReplayNewCheckpoint();
}

void NewCheckpointFlushed() {
  V8RecordReplayNewCheckpointFlushed();
}

void SetScriptAlive(bool alive) {
  V8RecordReplaySetScriptAlive(alive);
}

uint64_t NewBookmark() {
  return V8RecordReplayNewBookmark();
}

void OnAnnotation(const char* kind, const char* contents) {
  V8RecordReplayOnAnnotation(kind, contents);
}

void OnNetworkRequest(const char* id, const char* kind, uint64_t bookmark) {
  V8RecordReplayOnNetworkRequest(id, kind, bookmark);
}

void OnNetworkRequestEvent(const char* id) {
  V8RecordReplayOnNetworkRequestEvent(id);
}

void OnNetworkStreamStart(const char* id, const char* kind, const char* parentId) {
  V8RecordReplayOnNetworkStreamStart(id, kind, parentId);
}

void OnNetworkStreamData(const char* id, size_t offset, size_t length, uint64_t bookmark) {
  V8RecordReplayOnNetworkStreamData(id, offset, length, bookmark);
}

void OnNetworkStreamEnd(const char* id, size_t length) {
  V8RecordReplayOnNetworkStreamEnd(id, length);
}

bool AreEventsDisallowed(const char* why) {
  return V8RecordReplayAreEventsDisallowed(why);
}

void BeginDisallowEvents() {
  V8RecordReplayBeginDisallowEvents();
}

void BeginDisallowEventsWithLabel(const char* label) {
  V8RecordReplayBeginDisallowEventsWithLabel(label);
}

void EndDisallowEvents() {
  V8RecordReplayEndDisallowEvents();
}

bool AreEventsPassedThrough(const char* why) {
  return V8RecordReplayAreEventsPassedThrough(why);
}

void BeginPassThroughEvents() {
  V8RecordReplayBeginPassThroughEvents();
}

void EndPassThroughEvents() {
  V8RecordReplayEndPassThroughEvents();
}

bool FeatureEnabled(const char* feature, const char* subfeature) {
  return V8RecordReplayFeatureEnabled(feature, subfeature);
}

bool HasDisabledFeatures() {
  return V8RecordReplayHasDisabledFeatures();
}

void GetCurrentJSStack(std::string* stackTrace) {
  return V8RecordReplayGetCurrentJSStack(stackTrace);
}

void BrowserEvent(const char* name, const base::Value& info) {
  std::string json;
  base::JSONWriter::Write(info, &json);
  V8RecordReplayBrowserEvent(name, json.c_str());
}

bool HasDivergedFromRecording() {
  return V8RecordReplayHasDivergedFromRecording();
}

bool AllowSideEffects() {
  return V8RecordReplayAllowSideEffects();
}

void RegisterPointer(const char* name, const void* ptr) {
  V8RecordReplayRegisterPointer(name, ptr);
}

void UnregisterPointer(const void* ptr) {
  V8RecordReplayUnregisterPointer(ptr);
}

int PointerId(const void* ptr) {
  return V8RecordReplayPointerId(ptr);
}

void* IdPointer(int id) {
  return V8RecordReplayIdPointer(id);
}

void OnEvent(const char* aEvent, bool aBefore) {
  V8RecordReplayOnEvent(aEvent, aBefore);
}
void OnMouseEvent(const char* kind,
                  size_t clientX,
                  size_t clientY,
                  bool synthetic) {
  V8RecordReplayOnMouseEvent(kind, clientX, clientY, synthetic);
}
void OnKeyEvent(const char* kind,
                const char* key,
                bool synthetic) {
  V8RecordReplayOnKeyEvent(kind, key, synthetic);
}
void OnNavigationEvent(const char* kind, const char* url) {
  V8RecordReplayOnNavigationEvent(kind, url);
}

AutoLockMaybeEventsDisallowed::AutoLockMaybeEventsDisallowed(
        base::Lock& lock)
    : lock_(lock) {
  if (AreEventsDisallowed()) {
    AutoPassThroughEvents pt;
    lock.Acquire();
  } else {
    lock.Acquire();
  }
}

AutoLockMaybeEventsDisallowed::~AutoLockMaybeEventsDisallowed() {
  lock_.Release();
}

AutoUnlockMaybeEventsDisallowed::AutoUnlockMaybeEventsDisallowed(base::Lock& lock) : lock_(lock) {
  lock_.Release();
}

AutoUnlockMaybeEventsDisallowed::~AutoUnlockMaybeEventsDisallowed() {
  if (AreEventsDisallowed()) {
    AutoPassThroughEvents pt;
    lock_.Acquire();
  } else {
    lock_.Acquire();
  }
}

bool DependencyGraphEnabled() {
  return V8RecordReplayUpdateDependencyGraph();
}

int NewDependencyGraphNode(const char* json) {
  return V8RecordReplayNewDependencyGraphNode(json);
}

void AddDependencyGraphEdge(int source, int target, const char* json) {
  V8RecordReplayAddDependencyGraphEdge(source, target, json);
}

void BeginDependencyExecution(int node) {
  V8RecordReplayBeginDependencyExecution(node);
}

void EndDependencyExecution() {
  V8RecordReplayEndDependencyExecution();
}

AutoMarkerDependencyExecution::AutoMarkerDependencyExecution(const char* reason, const char* name) {
  if (DependencyGraphEnabled()) {
    base::DictValue info;
    info.Set("kind", "marker");
    info.Set("reason", reason);
    info.Set("name", name);
    std::string json;
    base::JSONWriter::Write(info, &json);
    int node_id = NewDependencyGraphNode(json.c_str());
    BeginDependencyExecution(node_id);
  }
}

AutoMarkerDependencyExecution::~AutoMarkerDependencyExecution() {
  if (DependencyGraphEnabled())
    EndDependencyExecution();
}

bool IsMainThread() {
  return V8IsMainThread();
}

static int gNextMainThreadId = 1;

static bool CheckNewId(const char* name) {
  if (!IsRecordingOrReplaying()) {
    // Don't track anything.
    return false;
  }
  if (HasDivergedFromRecording()) {
    // Everything is allowed when explicitly diverged.
    return true;
  }
  if (AreEventsDisallowed()) {
    // IDs can be created when events are disallowed when our own scripts
    // create URL objects. This would be nice to improve.
    if (!IsInReplayCode()) {
      Warning("NewIdWithEventsDisallowed", {{"name", name}});
    }
    return false;
  }
  Assert("NewId %s", name);
  return true;
}

int NewIdMainThread(const char* name) {
  if (!CheckNewId(name)) {
    return 0;
  }
  if (!V8IsMainThread()) {
    UNSAFE_TODO(fprintf(stderr, "NewIdMainThread not main thread: %s\n", name));
    CHECK(V8IsMainThread());
  }
  return gNextMainThreadId++;
}

static std::atomic<int> gNextAnyThreadId{1};

int NewIdAnyThread(const char* name) {
  if (!CheckNewId(name)) {
    return 0;
  }
  return (int)RecordReplayValue("NewId", (uintptr_t)gNextAnyThreadId++);
}

void Crash(const char* format, ...) {
  va_list args;
  va_start(args, format);
  V8RecordReplayCrash(format, args);
  va_end(args);
}

bool IsInReplayCode(const char* why) {
  return V8RecordReplayIsInReplayCode(why);
}

void EnterReplayCode() {
  V8RecordReplayEnterReplayCode();
}

void ExitReplayCode() {
  V8RecordReplayExitReplayCode();
}

bool AreEventsUnavailable(const char* why) {
  return AreEventsDisallowed(why) || HasDivergedFromRecording();
}

static size_t gMaxOldSpaceMb;

void SetMaxOldSpaceMb(size_t mb) {
  gMaxOldSpaceMb = mb;
}

size_t MaxOldSpaceMb() {
  return gMaxOldSpaceMb;
}

AutoMarkReplayCode::AutoMarkReplayCode() {
  V8RecordReplayEnterReplayCode();
}

AutoMarkReplayCode::~AutoMarkReplayCode() {
  V8RecordReplayExitReplayCode();
}

AutoAssertBufferAllocations::AutoAssertBufferAllocations(const char* issueLabel) {
  V8RecordReplayBeginAssertBufferAllocations(issueLabel);
}
AutoAssertBufferAllocations::~AutoAssertBufferAllocations() {
  V8RecordReplayEndAssertBufferAllocations();
}

void AddOrderedSRWLock(const char* name, void* lock) {
  V8RecordReplayAddOrderedSRWLock(name, lock);
}

void RemoveOrderedSRWLock(void* lock) {
  V8RecordReplayRemoveOrderedSRWLock(lock);
}

void MaybeTerminate(void (*callback)(void*), void* data) {
  V8RecordReplayMaybeTerminate(callback, data);
}

void FinishRecording() {
  V8RecordReplayFinishRecording();
}

std::vector<std::string> *gPseudoStack = nullptr;

static std::vector<std::string>* GetPseudoStack() {
  if (!gPseudoStack) {
    gPseudoStack = new std::vector<std::string>();
  }
  return gPseudoStack;
}

size_t PushPseudoStackFrame(const char* name) {
  std::vector<std::string>* pseudoStack = GetPseudoStack();
  pseudoStack->push_back(name);
  return pseudoStack->size();
}
void PopPseudoStackFrame(const char* name, size_t id) {
  CHECK(gPseudoStack != nullptr);
  CHECK(gPseudoStack->size() == id);
  CHECK(gPseudoStack->back() == name);
  gPseudoStack->pop_back();
}
const std::vector<std::string>& ReadPseudoStack() {
  return *GetPseudoStack();
}

std::string ReadPseudoStackString() {
  if (!gPseudoStack) {
    return std::string();
  }
  std::ostringstream out;
  size_t idx = 0;
  for (auto& frame : *gPseudoStack) {
    out << (idx > 0 ? " => " : "") << frame;
    idx++;
  }
  return out.str();
}

} // namespace recordreplay
