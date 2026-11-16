// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/protected_memory.h"

#include <sys/mman.h>

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_ANDROID)
#include <sys/resource.h>
#endif  // BUILDFLAG(IS_LINUX)

#if BUILDFLAG(IS_MAC)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif  // BUILDFLAG(IS_MAC)

#include "base/bits.h"
#include "base/memory/page_size.h"
#include "base/record_replay.h"

namespace base {

#if BUILDFLAG(PROTECTED_MEMORY_ENABLED)
namespace {

bool SetMemory(void* start, void* end, int prot) {
  CHECK(end > start);
  const uintptr_t page_start =
      bits::AlignDown(reinterpret_cast<uintptr_t>(start), GetPageSize());
  return mprotect(reinterpret_cast<void*>(page_start),
                  reinterpret_cast<uintptr_t>(end) - page_start, prot) == 0;
}

}  // namespace

namespace internal {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_ANDROID)
void CheckMemoryReadOnly(const void* ptr) {
  const uintptr_t page_start =
      bits::AlignDown(reinterpret_cast<uintptr_t>(ptr), GetPageSize());

  // Note: We've casted away const here, which should not be meaningful since
  // if the memory is written to we will abort immediately.
  //
  // [Replay] This call is a memory-protection PROBE, not a query: it hands the
  // kernel the protected page as getrlimit's output buffer and expects to be
  // refused.  The driver records getrlimit as
  // `OutParam<1, Constant::sizeof_rlimit>`, so a replay replaying that call
  // writes the recorded rlimit back into the read-only page and faults.
  // Passing the call through is not an option either: the replay has no
  // binding for getrlimit64 and dies with "Call to unbound symbol".  The
  // check verifies nothing about the program, only that this process's own
  // mprotect took effect, so skip it on both sides -- symmetric, and nothing
  // is recorded.  DSRC-235.
  if (recordreplay::IsRecordingOrReplaying()) {
    return;
  }

  int result =
      getrlimit(RLIMIT_NPROC, reinterpret_cast<struct rlimit*>(page_start));
  CHECK(result == -1 && errno == EFAULT);
}
#elif BUILDFLAG(IS_MAC)
void CheckMemoryReadOnly(const void* ptr) {
  mach_port_t object_name;
  vm_region_basic_info_64 region_info;
  mach_vm_size_t size = 1;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;

  kern_return_t kr = mach_vm_region(
      mach_task_self(), reinterpret_cast<mach_vm_address_t*>(&ptr), &size,
      VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&region_info),
      &count, &object_name);
  CHECK(kr == KERN_SUCCESS && region_info.protection == VM_PROT_READ);
}
#endif
}  // namespace internal

bool AutoWritableMemoryBase::SetMemoryReadWrite(void* start, void* end) {
  return SetMemory(start, end, PROT_READ | PROT_WRITE);
}

bool AutoWritableMemoryBase::SetMemoryReadOnly(void* start, void* end) {
  return SetMemory(start, end, PROT_READ);
}
#endif  // BUILDFLAG(PROTECTED_MEMORY_ENABLED)
}  // namespace base
