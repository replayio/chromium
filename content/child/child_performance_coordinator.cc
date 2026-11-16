// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/child/child_performance_coordinator.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/memory/weak_ptr.h"
#include "base/record_replay.h"
#include "base/sequence_checker.h"
#include "components/performance_manager/public/mojom/coordination_unit.mojom.h"
#include "components/performance_manager/scenario_api/performance_scenarios.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/perfetto/include/perfetto/tracing/track.h"

namespace content {

ChildPerformanceCoordinator::ChildPerformanceCoordinator() = default;

ChildPerformanceCoordinator::~ChildPerformanceCoordinator() = default;

mojo::PendingReceiver<performance_manager::mojom::ChildProcessCoordinationUnit>
ChildPerformanceCoordinator::InitializeAndPassReceiver() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto receiver = coordination_unit_.BindNewPipeAndPassReceiver();
  coordination_unit_->InitializeChildProcessCoordination(
      base::BindOnce(
          &ChildPerformanceCoordinator::OnInitializeChildProcessCoordination,
          weak_factory_.GetWeakPtr()));
  return receiver;
}

void ChildPerformanceCoordinator::OnInitializeChildProcessCoordination(
    base::ReadOnlySharedMemoryRegion global_region,
    base::ReadOnlySharedMemoryRegion process_region) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  using performance_scenarios::ScenarioScope;
  // [Replay] These regions are memory the BROWSER keeps writing: plain stores
  // into a shared mapping, which no recording sees.  A replay has no browser,
  // so its copy never changes, and every branch on the scenario goes one way
  // while recording and the other way while replaying.  Measured on the main
  // thread's per-task path: MainThreadSchedulerImpl::OnTaskStarted ->
  // PerformanceScenarioObserverList::NotifyIfScenarioChanged saw a changed
  // scenario in the recording only, and its Notify took a recorded lock the
  // replay never took.
  //
  // Recording each load would not hold: the loads are spread over several
  // call sites on any thread, some of them conditional.  So don't map the
  // memory at all.  Without a mapping the observer lists stay uninitialized
  // and never notify, and GetLoadingScenario/GetInputScenario read the
  // constant default state (no page loading, no input) on both sides.
  //
  // The scenario is a hint: the only consumer in a child process is the main
  // thread scheduler, which forwards it to V8's loading/input-handling
  // heuristics, and only under two features that are off by default.
  // DSRC-357.
  if (recordreplay::IsRecordingOrReplaying()) {
    return;
  }
  if (global_region.IsValid()) {
    global_scenario_memory_.emplace(ScenarioScope::kGlobal,
                                    std::move(global_region));
  }
  if (process_region.IsValid()) {
    process_scenario_memory_.emplace(ScenarioScope::kCurrentProcess,
                                     std::move(process_region));
  }
}

}  // namespace content
