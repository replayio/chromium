// Copyright 2020 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_DISPLAY_RECORD_REPLAY_RENDER_H_
#define COMPONENTS_VIZ_SERVICE_DISPLAY_RECORD_REPLAY_RENDER_H_

#include "base/memory/scoped_refptr.h"
#include "cc/trees/proxy_main.h"
#include "components/viz/common/resources/resource_id.h"
#include "components/viz/common/surfaces/local_surface_id.h"
#include "components/viz/common/quads/compositor_frame.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPixmap.h"

namespace gpu {
class SharedImageInterface;
}

namespace recordreplay {

// When recording, renderer processes generate compositor frames in the usual
// way and send them on to the GPU process for drawing to the screen. When
// replaying (and optionally when recording, for debugging) the process
// additionally sends these frames to an in process renderer for updating an
// in process buffer with the data the process is currently drawing. This data
// can then be encoded to base64 images and reported to the record/replay
// driver and sent to clients inspecting the recording.

// Initialize relevant render logic.
void InitPaintCallback();

// Called on the main thread when changes have been committed to the layer tree,
// before they are handed to the compositor thread. Returns the bookmark of the
// paint it starts, which the commit carries, or 0.
size_t OnCommitPaint();

// Called on the compositor thread when the main thread's notification about
// the commit, whose tree has the given source frame number, carrying the
// bookmark OnCommitPaint returned.
void OnReadyToCommit(const void* compositor,
                     int source_frame_number,
                     size_t bookmark);

// Called on the compositor thread before drawing the active tree, which has
// the given source frame number.
void OnDrawSourceFrame(const void* compositor, int source_frame_number);

// Called on the compositor thread when the compositor is destroyed.
void OnCompositorDestroyed(const void* compositor);

// Called when a CompositorFrame is being submitted to the GPU process.
void SubmitCompositorFrame(const viz::LocalSurfaceId& local_surface_id,
                           const viz::CompositorFrame& frame);

// Called after a frame passed to SubmitCompositorFrame has been sent to the
// GPU process, which destroys the frame.
void FinishSubmittedCompositorFrame();

// Called to populate a bitmap with information for the given resource in the current frame.
// The pixels are those of the software shared image the resource carries.
bool PopulateSkBitmapWithResource(SkBitmap* sk_bitmap, viz::ResourceId resource_id);

// Called on the compositor thread when painting to the software output device has finished.
void OnPaintFinished(const SkPixmap& pixmap);

// Called on the compositor thread when repainting has finished, which may or may not
// have actually performed a paint.
void OnRepaintFinished();

// Set the proxy which will be used for triggering repaints from the main thread.
void SetCompositorProxy(cc::ProxyMain* proxy);

// Called when a compositor proxy has been destroyed and can't be used to trigger repaints.
void CompositorProxyDestroyed(cc::ProxyMain* proxy);

// What a repaint's tiles are made with when the compositor has never had a
// frame sink, as at a pause before the page's first one was made: tiles are
// rastered into shared images, and the interface for them comes with a frame
// sink. The renderer says how to get one. Main thread.
using RepaintSharedImageInterfaceGetter =
    scoped_refptr<gpu::SharedImageInterface> (*)();
void SetRepaintSharedImageInterfaceGetter(
    RepaintSharedImageInterfaceGetter getter);
scoped_refptr<gpu::SharedImageInterface> GetRepaintSharedImageInterface();

gfx::Size GetCurrentViewportPixelSizeImpl();

} // namespace recordreplay

#endif // COMPONENTS_VIZ_SERVICE_DISPLAY_RECORD_REPLAY_RENDER_H_
