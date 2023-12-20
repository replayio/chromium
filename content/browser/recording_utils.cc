#include <memory>
#include <vector>

#include "base/bind.h"
#include "base/callback_forward.h"
#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/memory/ref_counted.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "content/public/browser/browser_child_process_host_iterator.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
// #include "content/public/browser/child_process_data.h"
// #include "content/public/browser/gpu_utils.h"
#include "content/public/browser/render_process_host.h"
// #include "content/public/common/child_process_host.h"
#include "content/public/common/content_switches.h"
// #include "content/public/common/recording_utils.h"

namespace content {

namespace {

// A refcounted class that runs a closure once it's destroyed.
class RefCountedScopedClosureRunner
    : public base::RefCounted<RefCountedScopedClosureRunner> {
 public:
  RefCountedScopedClosureRunner(base::OnceClosure callback);

 private:
  friend class base::RefCounted<RefCountedScopedClosureRunner>;
  ~RefCountedScopedClosureRunner() = default;

  base::ScopedClosureRunner destruction_callback_;
};

RefCountedScopedClosureRunner::RefCountedScopedClosureRunner(
    base::OnceClosure callback)
    : destruction_callback_(std::move(callback)) {}

}  // namespace

void RecordReplayAskAllRecordingChildrenToFinishRecording(base::OnceClosure callback) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kSingleProcess)) {
    // TODO(toshok) not sure - maybe we call recordreplay::FinishRecording directly?
    LOG(ERROR) << "We're single threaded!";
    return;
  }

  auto closure_runner =
      base::MakeRefCounted<RefCountedScopedClosureRunner>(std::move(callback));

  // Ask all the renderer processes to finish their recordings.
  for (content::RenderProcessHost::iterator i(content::RenderProcessHost::AllHostsIterator());
       !i.IsAtEnd(); i.Advance()) {
    DCHECK(!i.GetCurrentValue()->GetProcess().is_current());
    if (!i.GetCurrentValue()->IsInitializedAndNotDead())
      continue;
    i.GetCurrentValue()->FinishRecording(base::BindOnce(
        [](scoped_refptr<RefCountedScopedClosureRunner>) {}, closure_runner));
  }
}

void RecordReplayAskChildrenToFinishRecording(base::span<RenderProcessHost*> hosts, base::OnceClosure callback) {
  auto closure_runner =
      base::MakeRefCounted<RefCountedScopedClosureRunner>(std::move(callback));

  for (const auto& host : hosts) {
        DCHECK(!host->GetProcess().is_current());
        if (!host->IsInitializedAndNotDead())
        continue;
        host->FinishRecording(base::BindOnce(
            [](scoped_refptr<RefCountedScopedClosureRunner>) {}, closure_runner));
    }
}