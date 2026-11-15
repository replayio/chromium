// For use in situations when the recordreplay:: namespace isn't available
// due to build dependency ordering.

#if !BUILDFLAG(IS_WIN)
#include <dlfcn.h>
#else
#include <windows.h>
#endif

using RecordReplayVoidFn = void (*)();

static RecordReplayVoidFn LookupRecordReplaySymbol(const char* name) {
#if !BUILDFLAG(IS_WIN)
  void* fnptr = dlsym(RTLD_DEFAULT, name);
#else
  HMODULE module = GetModuleHandleA("windows-recordreplay.dll");
  void* fnptr = module ? (void*)GetProcAddress(module, name) : nullptr;
#endif
  return reinterpret_cast<RecordReplayVoidFn>(
      fnptr ? fnptr : reinterpret_cast<void*>(1));
}

struct RecordReplayAutoPassThroughEvents {
  // Function pointers (not data pointers) — intentionally not `raw_ptr<T>`,
  // which only applies to data pointers and does not support `void`.
  RecordReplayVoidFn fnBegin;
  RecordReplayVoidFn fnEnd;
  bool didBegin;

  RecordReplayAutoPassThroughEvents() {
    // Cache our functions, since begin passthrough events will not allow us to
    // invoke our own op_dlsym function which can actually find our symbols.
    fnBegin = LookupRecordReplaySymbol("RecordReplayBeginPassThroughEvents");
    fnEnd = LookupRecordReplaySymbol("RecordReplayEndPassThroughEvents");

    if (reinterpret_cast<void*>(fnBegin) != reinterpret_cast<void*>(1)) {
      fnBegin();
    }
  }

  ~RecordReplayAutoPassThroughEvents() {
    if (reinterpret_cast<void*>(fnEnd) != reinterpret_cast<void*>(1)) {
      fnEnd();
    }
  }
};
