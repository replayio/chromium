// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/task_runner.h"

#include <utility>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/threading/post_task_and_reply_impl.h"
#include "base/time/time.h"

#if !BUILDFLAG(IS_WIN)
#include <dlfcn.h>
#else
#include <windows.h>
#endif

static void* LookupRecordReplaySymbol(const char* name) {
#if !BUILDFLAG(IS_WIN)
  void* fnptr = dlsym(RTLD_DEFAULT, name);
#else
  HMODULE module = GetModuleHandleA("windows-recordreplay.dll");
  void* fnptr = module ? (void*)GetProcAddress(module, name) : nullptr;
#endif
  return fnptr ? fnptr : reinterpret_cast<void*>(1);
}

static bool RecordReplayRegisterPointer(const char* name, void* ptr) {
  static void* fnptr;
  if (!fnptr) {
    fnptr = LookupRecordReplaySymbol("RecordReplayRegisterPointerWithName");
  }
  if (fnptr != reinterpret_cast<void*>(1)) {
    reinterpret_cast<void(*)(const char*, void*)>(fnptr)(name, ptr);
    return true;
  }
  return false;
}

static void RecordReplayUnregisterPointer(void* ptr) {
  static void* fnptr;
  if (!fnptr) {
    fnptr = LookupRecordReplaySymbol("RecordReplayUnregisterPointer");
  }
  if (fnptr != reinterpret_cast<void*>(1)) {
    reinterpret_cast<void(*)(void*)>(fnptr)(ptr);
  }
}

namespace base {

bool TaskRunner::PostTask(const Location& from_here, OnceClosure task) {
  return PostDelayedTask(from_here, std::move(task), base::TimeDelta());
}

bool TaskRunner::PostTaskAndReply(const Location& from_here,
                                  OnceClosure task,
                                  OnceClosure reply) {
  return internal::PostTaskAndReplyImpl(
      [this](const Location& location, OnceClosure task) {
        return PostTask(location, std::move(task));
      },
      from_here, std::move(task), std::move(reply));
}

TaskRunner::TaskRunner() {
  // Pointer registration is needed for sorting in various places.
  if (RecordReplayRegisterPointer("TaskRunner", this)) {
    // Leak task runners when recording/replaying, as they can be destroyed at
    // non-deterministic points and this is simpler than ordering uses of their
    // reference count.
    AddRef();
  }
}

TaskRunner::~TaskRunner() {
  RecordReplayUnregisterPointer(this);
}

void TaskRunner::OnDestruct() const {
  delete this;
}

void TaskRunnerTraits::Destruct(const TaskRunner* task_runner) {
  task_runner->OnDestruct();
}

}  // namespace base
