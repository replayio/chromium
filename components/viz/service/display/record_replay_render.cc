// Copyright 2020 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string_view>
#include <optional>
#include "components/viz/service/display/record_replay_render.h"

#include <algorithm>

#include "base/base64.h"
#include "base/no_destructor.h"
#include "base/record_replay.h"
#include "base/record_replay_paint_surface.h"
#include "base/record_replay_render_interface.h"
#include "base/strings/stringprintf.h"
#include "cc/animation/animation_events.h"
#include "cc/trees/compositor_commit_data.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display/software_renderer.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "components/viz/common/resources/shared_image_format_utils.h"
#include "components/viz/service/display_embedder/software_output_surface.h"
#include "ui/gfx/codec/jpeg_codec.h"

namespace blink {
extern bool RecordReplayStateEnsureInitialized();
}

namespace recordreplay {

extern bool IsMainThread();

static viz::SoftwareOutputSurface* gOutputSurface;
static viz::SoftwareRenderer* gRenderer;

// For now we only support drawing a single surface, which is the first one
// which the process tried to draw.  This variable is reset when the target
// surface is expected to change, such as after a navigation, so that the new
// surface is not ignored: other threads request that with
// `recordreplay::DoResetPaintSurface` and the next submitted frame applies it.
// See https://linear.app/replay/issue/RUN-2400
static viz::LocalSurfaceId* gSurfaceId;

// Embed token of `gSurfaceId`, which identifies the window owning the paint
// surface, for repaints on the main thread: `gSurfaceId` belongs to the
// compositor thread.
static std::atomic<const base::UnguessableToken*> gSurfaceEmbedToken;

static void InitializeRenderer() {
  std::unique_ptr<viz::SoftwareOutputDevice> output_device =
      std::make_unique<viz::SoftwareOutputDevice>();
  gOutputSurface = new viz::SoftwareOutputSurface(std::move(output_device));

  gRenderer = new viz::SoftwareRenderer(new viz::RendererSettings(),
                                        new viz::DebugRendererSettings(),
                                        gOutputSurface,
                                        nullptr,
                                        nullptr);

  recordreplay::SetGetCurrentViewportPixelSizeCallback(GetCurrentViewportPixelSizeImpl);
}

static std::string SurfaceIdString(const viz::LocalSurfaceId& local_surface_id) {
  // LocalSurfaceId has ToString(), but without verbose logging the token is truncated.
  return base::StringPrintf("%u:%u:%s",
                            local_surface_id.parent_sequence_number(),
                            local_surface_id.child_sequence_number(),
                            local_surface_id.embed_token().ToString().c_str());
}

// Current compositor frame.
static const viz::CompositorFrame* gCurrentFrame;

void SubmitCompositorFrame(const viz::LocalSurfaceId& local_surface_id,
                           const viz::CompositorFrame& frame) {
  CHECK(IsRecordingOrReplaying());

  if (TakePaintSurfaceReset()) {
    gSurfaceId = nullptr;
  }

  if (!gSurfaceId) {
    gSurfaceId = new viz::LocalSurfaceId(local_surface_id);
    gSurfaceEmbedToken =
        new base::UnguessableToken(local_surface_id.embed_token());
    InitializeRenderer();
  }

  // Surfaces are allowed to have different parent sequence numbers from the
  // original one, and newer child sequence numbers: the widget allocates one
  // whenever its frame sink is recreated, e.g. after the GPU process restarts.
  if (gSurfaceId->embed_token() == local_surface_id.embed_token() &&
      local_surface_id.child_sequence_number() >
          gSurfaceId->child_sequence_number()) {
    *gSurfaceId = local_surface_id;
  }
  if (gSurfaceId->child_sequence_number() != local_surface_id.child_sequence_number() ||
      gSurfaceId->embed_token() != local_surface_id.embed_token()) {
    Print("Ignoring composite to unknown surface %s, expected %s",
          SurfaceIdString(local_surface_id).c_str(),
          SurfaceIdString(*gSurfaceId).c_str());
    return;
  }

  viz::AggregatedRenderPassList render_passes;
  for (const auto& pass : frame.render_pass_list) {
    render_passes.push_back(pass->DeepCopyAggregated());
  }

  gCurrentFrame = &frame;

  gRenderer->DrawFrame(&render_passes,
                       1,
                       frame.size_in_pixels(),
                       gfx::DisplayColorSpaces(),
                       viz::SurfaceDamageRectList(),
                       viz::TrackedElementRects());

  gCurrentFrame = nullptr;
}

bool PopulateSkBitmapWithResource(SkBitmap* sk_bitmap, viz::ResourceId resource_id) {
  CHECK(gCurrentFrame);

  const viz::TransferableResource* transferable = nullptr;
  for (const viz::TransferableResource& resource : gCurrentFrame->resource_list) {
    if (resource.id == resource_id) {
      transferable = &resource;
      break;
    }
  }

  recordreplay::AssertMaybeEventsDisallowed("[RUN-2847-3001] recordreplay::PopulateSkBitmapWithResource A %d", !!transferable);

  if (!transferable) {
    return false;
  }

  // [RecordReplay] Read the tile's pixels from the frame's own resource.
  //
  // Up to M133 this came from a table the raster provider filled, via
  // NotifyRasterBuffer in cc/raster/bitmap_raster_buffer_provider.cc, and when
  // upstream removed the SharedBitmap identity scheme at M134 and the provider
  // itself at M136 the notification was dropped as a reversible gap rather than
  // ported. This is that reversal, and it needs no producer side: the frame's
  // resource already carries the image, built by ResourcePool::PrepareForExport
  // from the backing's shared image, so the consumer can map it directly.
  //
  // Measured cost of the gap, which is what motivated restoring it: in
  // render/repaint-at-endpoint-without-frame-sink the repaint's frame carries
  // eighteen software tile quads and this function was reached for every one of
  // them, returned false, and SoftwareRenderer skipped each quad silently, so
  // the page's unchanged background showed through instead of the committed
  // DOM change.
  //
  // The colour type comes from the resource's own format rather than
  // MakeN32Premul, which the dropped code used. The software tiles here are
  // BGRA_8888 while N32 is RGBA on this platform, and that swap would turn the
  // case's expected rgb(200,40,40) into rgb(40,40,200) -- which is the value it
  // fails with when nothing is drawn at all, so the mistake would read as a
  // no-op rather than as a bug. DSRC-065.
  std::unique_ptr<gpu::ClientSharedImage::ScopedMapping> mapping;
  void* pixels = nullptr;
  size_t stride = 0;
  if (const scoped_refptr<gpu::ClientSharedImage>& image =
          transferable->shared_image();
      HasDivergedFromRecording() && image && transferable->GetIsSoftware()) {
    mapping = image->Map();
    if (mapping) {
      base::span<uint8_t> plane = mapping->GetMemoryForPlane(0);
      pixels = plane.data();
      stride = mapping->Stride(0);
    }
  }

  recordreplay::AssertMaybeEventsDisallowed("[RUN-2847-3001] recordreplay::PopulateSkBitmapWithResource B %d", !!pixels);
  if (!pixels) {
    return false;
  }

  SkImageInfo info = SkImageInfo::Make(
      transferable->GetSize().width(), transferable->GetSize().height(),
      viz::ToClosestSkColorType(transferable->GetFormat()),
      kPremul_SkAlphaType);

  // Copy rather than alias. installPixels does not copy, and the mapping it
  // would alias is owned by the local above, which is destroyed when this
  // function returns, so the bitmap would be left pointing into unmapped
  // memory and nothing here guarantees the renderer reads it before then.
  if (!sk_bitmap->tryAllocPixels(info)) {
    return false;
  }
  // Skia does the copy, because a hand-rolled one trips the tree's
  // -Wunsafe-buffer-usage.
  return sk_bitmap->writePixels(SkPixmap(info, pixels, stride));
}

extern "C" size_t V8RecordReplayPaintStart();
extern "C" void V8RecordReplayPaintFinished(size_t bookmark);
extern "C" void V8RecordReplaySetPaintCallback(char* (*callback)(const char*, int));

const SkPixmap* gCurrentPixmap;

static char* EncodeBitmapContents(const char* mime_type, int jpeg_quality) {
  CHECK(gCurrentPixmap);

  if (std::string_view(mime_type) != "image/jpeg") {
    // NYI
    return nullptr;
  }

  std::optional<std::vector<uint8_t>> data = gfx::JPEGCodec::Encode(
      *gCurrentPixmap, jpeg_quality, SkJpegEncoder::Downsample::k444);
  if (!data) {
    Print("Error: JPEG encoding failed");
    return nullptr;
  }

  std::string encoded = base::Base64Encode(*data);
  return strdup(encoded.c_str());
}

static char* PaintWhenDiverged(const char* mime_type, int jpeg_quality);

static char* PaintCallback(const char* mime_type, int jpeg_quality) {
  // The paint callback can be invoked either when we call RecordReplayPaintFinished
  // on the compositor thread, or at any time after diverging from the recording and
  // we are on the main thread. These invocations need different handling.
  if (HasDivergedFromRecording()) {
    return PaintWhenDiverged(mime_type, jpeg_quality);
  }

  return EncodeBitmapContents(mime_type, jpeg_quality);
}

// Paints generally occur in the following way:
//
// 1. Main thread commits a paint in ProxyMain::BeginMainFrame, posts a task
//    to run ProxyImpl::NotifyReadyToCommitOnImpl on the compositor thread,
//    and blocks until the completion event associated with that task is signaled.
//
// 2. Compositor thread runs ProxyImpl::NotifyReadyToCommitOnImpl and at some
//    point signals the completion event so the main thread can resume executing.
//
// 3. Compositor thread finishes painting the frame.
//
// The paint finished in #3 needs to be associated with the bookmark created in #1.
// The state below is used to keep track of the bookmark for the current paint
// in a consistent way between recording and replaying. This isn't necessarily
// correct, though --- #2 and #3 run in separate tasks on the compositor thread,
// so a frame could be marked ready to commit on the compositor thread before
// the previous frame finished painting. For now this is good enough, though.

// Bookmark which was last created on the main thread before blocking until the
// compositor thread is ready to commit.
static std::atomic<size_t> gCurrentPaintBookmark;

// Bookmark for the last point where a paint was committed on the main thread.
static std::atomic<size_t> gLastCommitBookmark;

void InitPaintCallback() {
  static bool hasPaints = false;
  if (!hasPaints) {
    hasPaints = true;
    V8RecordReplaySetPaintCallback(PaintCallback);
  }
}

void OnCommitPaint() {
  // Record/replay state has to be initialized before the first paint
  // starts, as a checkpoint must have been taken.
  if (blink::RecordReplayStateEnsureInitialized()) {
    gCurrentPaintBookmark = V8RecordReplayPaintStart();
  } else {
    gCurrentPaintBookmark = 0;
  }
}

void OnReadyToCommit() {
  gLastCommitBookmark.store(gCurrentPaintBookmark, std::memory_order_relaxed);
}

// How to encode repainted graphics.
static const char* gRepaintMimeType;
static int gRepaintJPEGQuality;

// Event to signal when repainting has finished.
static std::atomic<base::WaitableEvent*> gRepaintEvent;

// Encoded result of repainting.
static std::atomic<char*> gRepaintResult;

// Real size of |gRepaintResult|.
static std::atomic<gfx::Size> gDeviceViewportSize;

void OnPaintFinished(const SkPixmap& pixmap) {
  gCurrentPixmap = &pixmap;
  if (gOutputSurface) {
    // Obtain device size on impl thread.
    gDeviceViewportSize = gOutputSurface->software_device()->ReplayViewportPixelSize();
  }

  if (HasDivergedFromRecording()) {
    // If we've diverged from the recording then we're probably repainting,
    // and in any case don't need to notify the recorder that the paint finished.
    if (gRepaintEvent)
      gRepaintResult = EncodeBitmapContents(gRepaintMimeType, gRepaintJPEGQuality);
  } else {
    size_t bookmark = gLastCommitBookmark.load();
    if (bookmark) {
      V8RecordReplayPaintFinished(bookmark);
    }
  }

  gCurrentPixmap = nullptr;
}

void OnRepaintFinished() {
  base::WaitableEvent* e = gRepaintEvent.load();
  if (e != nullptr) {
    CHECK(HasDivergedFromRecording());
    e->Signal();
  }
}

static cc::ProxyMain* gCurrentCompositorProxy;

// The main thread's live compositor proxies. A process has one for each
// window it renders, e.g. a page and a popup it opened.
static std::vector<cc::ProxyMain*>* gCompositorProxies;

void SetCompositorProxy(cc::ProxyMain* proxy) {
  if (IsMainThread()) {
    gCurrentCompositorProxy = proxy;
    if (!gCompositorProxies) {
      gCompositorProxies = new std::vector<cc::ProxyMain*>();
    }
    if (!std::ranges::contains(*gCompositorProxies, proxy)) {
      gCompositorProxies->push_back(proxy);
    }
  }
}

void CompositorProxyDestroyed(cc::ProxyMain* proxy) {
  if (gCurrentCompositorProxy == proxy) {
    CHECK(IsMainThread());
    gCurrentCompositorProxy = nullptr;
  }
  if (gCompositorProxies && IsMainThread()) {
    std::erase(*gCompositorProxies, proxy);
  }
}

// The proxy to repaint with. Only frames submitted to the paint surface are
// drawn, so a repaint needs the proxy of the window which owns that surface.
// The current proxy is the one which last began a main frame, and with several
// windows in the process that can be another window's: after a page opens a
// popup, either window can own the paint surface while both keep running main
// frames. A repaint with the other window's proxy draws a frame which
// SubmitCompositorFrame ignores, and returns nothing.
//
// When no live proxy owns the paint surface (its window was closed and no
// frame has claimed the surface since), the current proxy is used and the
// repaint still returns nothing, unless a paint surface reset is pending.
//
// [RecordReplay] A proxy can also outlive its window while staying in that
// list. Upstream keeps a navigated-away widget's LayerTreeView for a second
// after the frame swap, under kDelayLayerTreeViewDeletionOnLocalSwap, hidden
// and without a frame sink, where the fork's M108 baseline disconnected it at
// once. Such a proxy still owns the paint surface until the new page submits
// its first frame, and it cannot repaint: ProxyMain::BeginMainFrame aborts
// before doing anything when its host is not visible, so the repaint's commit
// returns having done nothing and its draw never comes. Skipping it leaves the
// current proxy to repaint, which is what happened at M108 once the closed
// window's proxy was gone. Measured on the M158 leapfrog stack: the pause
// child that timed out in render/repaint-at-paint-points had taken the
// previous about:blank page's compositor, whose scheduler was not visible,
// while the five children of the same run that succeeded repainted the page's
// own. DSRC-426.
static cc::ProxyMain* RepaintCompositorProxy() {
  const base::UnguessableToken* embed_token = gSurfaceEmbedToken;
  if (embed_token && gCompositorProxies) {
    for (cc::ProxyMain* proxy : *gCompositorProxies) {
      if (proxy->RecordReplayLocalSurfaceId().embed_token() == *embed_token &&
          proxy->RecordReplayIsVisible()) {
        return proxy;
      }
    }
  }
  return gCurrentCompositorProxy;
}

static char* PaintWhenDiverged(const char* mime_type, int jpeg_quality) {
  CHECK(IsMainThread());

  gCurrentCompositorProxy = RepaintCompositorProxy();
  // [RecordReplay] A compositor whose host is not visible aborts its main
  // frame before doing anything, so a repaint on it can never draw. Fail now
  // rather than wait out the repaint's own timeout. DSRC-426.
  if (!gCurrentCompositorProxy ||
      !gCurrentCompositorProxy->RecordReplayIsVisible())
    return nullptr;

  gRepaintMimeType = mime_type;
  gRepaintJPEGQuality = jpeg_quality;

  // Update layout state on the main thread and commit a new paint.
  std::unique_ptr<cc::BeginMainFrameAndCommitState> begin_main_frame_state(
      new cc::BeginMainFrameAndCommitState);
  begin_main_frame_state->mutator_events = std::make_unique<cc::AnimationEvents>();
  begin_main_frame_state->commit_data = std::make_unique<cc::CompositorCommitData>();

  // The pause can leave a frame's tree pending behind one that was never
  // drawn, so the compositor has to be told about the repaint before its
  // commit, which waits for the pending tree.
  gCurrentCompositorProxy->RecordReplayRepaintPending();
  gCurrentCompositorProxy->BeginMainFrameWithBlocking(std::move(begin_main_frame_state), true);

  // A draw that an earlier repaint which timed out still forces doesn't
  // finish this one: there's no event to signal until this repaint's commit,
  // and the commit clears the earlier forced draw. Such a draw can still
  // signal the event after its repaint stopped waiting, so the event outlives
  // repaints.
  static base::NoDestructor<base::WaitableEvent> event;
  event->Reset();
  gRepaintResult = nullptr;
  gRepaintEvent = event.get();

  // Trigger a new frame on the compositor thread.
  gCurrentCompositorProxy->RecordReplayRepaint();

  // Wait for the repainting frame to complete.
  bool signaled = event->TimedWait(base::Milliseconds(3000));
  if (!signaled) {
    Print("[RuntimeError] Failed waiting to get a repaint.");
    gRepaintResult = nullptr;
  }

  gRepaintEvent = nullptr;
  return gRepaintResult;
}

gfx::Size GetCurrentViewportPixelSizeImpl() {
  return gDeviceViewportSize;
}

} // namespace recordreplay
