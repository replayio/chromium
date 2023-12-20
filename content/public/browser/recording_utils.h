// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_RECORDING_UTILS_H_
#define CONTENT_PUBLIC_BROWSER_RECORDING_UTILS_H_

#include "base/span.h"
#include "base/callback_forward.h"
#include "content/common/content_export.h"
#include "content/public/browser/render_process_host.h"

namespace content {

// Ask all the child processes to stop recording, make sure
// the recording is flushed, and calls |callback| once it's done.
CONTENT_EXPORT void RecordReplayAskAllChildrenToFinishRecording(
    base::OnceClosure callback);

// Same as above, but a subset of renderer children
CONTENT_EXPORT void RecordReplayAskChildrenToFinishRecording(
    base::span<RenderProcessHost*> render_processes,
    base::OnceClosure callback);

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_RECORDING_UTILS_H_
