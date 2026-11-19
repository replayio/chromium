// Copyright 2020 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display/record_replay_render.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/no_destructor.h"
#include "base/record_replay.h"
#include "base/record_replay_paint_surface.h"
#include "base/record_replay_render_interface.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "cc/animation/animation_events.h"
#include "cc/trees/compositor_commit_data.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/resources/shared_image_format_utils.h"
#include "components/viz/common/resources/transferable_resource.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display/software_renderer.h"
#include "components/viz/service/display_embedder/software_output_surface.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "ui/gfx/codec/jpeg_codec.h"

namespace blink {
extern bool RecordReplayStateEnsureInitialized();
}

namespace recordreplay {

extern bool IsMainThread();

// What a repaint makes tiles with when the compositor never had a frame sink.
static RepaintSharedImageInterfaceGetter gRepaintSharedImageInterfaceGetter;

void SetRepaintSharedImageInterfaceGetter(
    RepaintSharedImageInterfaceGetter getter) {
  gRepaintSharedImageInterfaceGetter = getter;
}

scoped_refptr<gpu::SharedImageInterface> GetRepaintSharedImageInterface() {
  if (!gRepaintSharedImageInterfaceGetter) {
    return nullptr;
  }
  return gRepaintSharedImageInterfaceGetter();
}

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
  gRenderer->Initialize();
  gRenderer->SetHasBackBuffers(true);

  recordreplay::SetGetCurrentViewportPixelSizeCallback(GetCurrentViewportPixelSizeImpl);
}

static std::string SurfaceIdString(const viz::LocalSurfaceId& local_surface_id) {
  // LocalSurfaceId has ToString(), but without verbose logging the token is truncated.
  return base::StringPrintf("%u:%u:%s",
                            local_surface_id.parent_sequence_number(),
                            local_surface_id.child_sequence_number(),
                            local_surface_id.embed_token().ToString().c_str());
}

// Current compositor frame, and the mappings of its tiles that are in use
// while it's drawn.
static const viz::CompositorFrame* gCurrentFrame;
static std::vector<std::unique_ptr<gpu::ClientSharedImage::ScopedMapping>>*
    gCurrentMappings;

// What a repaint's draw got as far as, said when it comes back with nothing.
static std::atomic<int> gRepaintFramesSubmitted;
static std::atomic<int> gRepaintFramesForOtherSurface;
static std::atomic<int> gRepaintFramesPainted;

void SubmitCompositorFrame(const viz::LocalSurfaceId& local_surface_id,
                           const viz::CompositorFrame& frame) {
  CHECK(IsRecordingOrReplaying());
  gRepaintFramesSubmitted++;

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
    gRepaintFramesForOtherSurface++;
    return;
  }

  viz::AggregatedRenderPassList render_passes;
  for (const auto& pass : frame.render_pass_list) {
    render_passes.push_back(pass->DeepCopyAggregated(frame.render_pass_list));
  }

  std::vector<std::unique_ptr<gpu::ClientSharedImage::ScopedMapping>> mappings;
  gCurrentFrame = &frame;
  gCurrentMappings = &mappings;

  gRenderer->DrawFrame(&render_passes,
                       1,
                       frame.size_in_pixels(),
                       gfx::DisplayColorSpaces(),
                       viz::SurfaceDamageRectList(),
                       viz::TrackedElementRects());

  gCurrentFrame = nullptr;
  gCurrentMappings = nullptr;
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

  // A software resource's pixels are in the shared image the resource itself
  // carries, in memory this process has mapped. (Chromium 108 rastered tiles
  // into shared bitmaps, which the frame named by an ID and which had to be
  // registered here as they were made.) A frame drawn in this process hasn't
  // been through mojo, so its resources still have their images.
  scoped_refptr<gpu::ClientSharedImage> image;
  if (transferable->GetIsSoftware() &&
      transferable->shared_image()->format().is_single_plane()) {
    image = transferable->shared_image();
  }

  recordreplay::AssertMaybeEventsDisallowed("[RUN-2847-3001] recordreplay::PopulateSkBitmapWithResource B %d", !!image);
  if (!image) {
    return false;
  }

  // The pixels stay mapped until the frame has been drawn.
  std::unique_ptr<gpu::ClientSharedImage::ScopedMapping> mapping = image->Map();
  if (!mapping) {
    return false;
  }
  gfx::Size size = mapping->Size();
  SkImageInfo info = SkImageInfo::Make(
      size.width(), size.height(),
      viz::ToClosestSkColorType(image->format()), kPremul_SkAlphaType);
  base::span<uint8_t> memory = mapping->GetMemoryForPlane(0);
  size_t stride = mapping->Stride(0);
  if (stride < info.minRowBytes() ||
      memory.size() < info.computeByteSize(stride)) {
    return false;
  }
  bool installed = sk_bitmap->installPixels(info, memory.data(), stride);
  gCurrentMappings->push_back(std::move(mapping));
  return installed;
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

// The bookmarks of the tree last committed and of the tree being drawn, which
// is the last one activated. Compositor thread. A finished paint is credited
// to the second: the compositor can hear of the next commit, and even make it,
// while it still draws the tree before, and across a navigation that next
// commit can be of an empty tree. Its bookmark would then be a paint point at
// which there is nothing to repaint.
static size_t gCommittedBookmark;
static size_t gActiveBookmark;

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

void OnCommitOnImpl() {
  gCommittedBookmark = gLastCommitBookmark.load();
}

void OnActivateOnImpl() {
  gActiveBookmark = gCommittedBookmark;
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
    gRepaintFramesPainted++;
    if (gRepaintEvent)
      gRepaintResult = EncodeBitmapContents(gRepaintMimeType, gRepaintJPEGQuality);
  } else {
    // The bookmarks come from the main thread, for each commit of any of the
    // process's compositors, while this thread draws: with several widgets (a
    // page and its popups) which commit this draw comes after isn't the same
    // on a replay. So what was read is recorded.
    size_t bookmark =
        RecordReplayValueIfAllowed("OnPaintFinished bookmark", gActiveBookmark);
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
static cc::ProxyMain* RepaintCompositorProxy() {
  const base::UnguessableToken* embed_token = gSurfaceEmbedToken;
  // After a navigation has asked for the paint surface to change, the owner
  // on record is the page that was left until the new page's first frame is
  // submitted. A repaint before then (at the new page's first paint, which is
  // credited to its commit) would draw the old page's window, which has no
  // layers left: a blank picture, when the new page gets a new window.
  if (IsPaintSurfaceResetPending()) {
    embed_token = nullptr;
  }
  if (embed_token && gCompositorProxies) {
    for (cc::ProxyMain* proxy : *gCompositorProxies) {
      if (proxy->RecordReplayLocalSurfaceId().embed_token() == *embed_token) {
        return proxy;
      }
    }
  }
  // No window owns the paint surface, or none has been drawn yet. The current
  // proxy is the one made or used last, which can be of a window that is
  // hidden and has no layers, and its repaint draws nothing or an old blank
  // frame. A window with layers is the one to draw: a visible one if there is
  // one, and the newest of those.
  if (gCompositorProxies &&
      (!gCurrentCompositorProxy ||
       !gCurrentCompositorProxy->RecordReplayHasRootLayer() ||
       !gCurrentCompositorProxy->RecordReplayIsVisible())) {
    cc::ProxyMain* with_layers = nullptr;
    cc::ProxyMain* visible_with_layers = nullptr;
    for (cc::ProxyMain* proxy : *gCompositorProxies) {
      if (!proxy->RecordReplayHasRootLayer()) {
        continue;
      }
      with_layers = proxy;
      if (proxy->RecordReplayIsVisible()) {
        visible_with_layers = proxy;
      }
    }
    if (visible_with_layers) {
      return visible_with_layers;
    }
    if (with_layers && (!gCurrentCompositorProxy ||
                        !gCurrentCompositorProxy->RecordReplayHasRootLayer())) {
      return with_layers;
    }
  }
  return gCurrentCompositorProxy;
}

static char* PaintWhenDiverged(const char* mime_type, int jpeg_quality) {
  CHECK(IsMainThread());

  // This repaint blocks on the compositor thread, which can be inside a
  // synchronous mojo call it entered before this child diverged: a GPU channel
  // flush for the sync tokens of the resources it was drawing with
  // (GpuChannelHost::VerifyFlush). A diverged child gets no replies, so that
  // call would never return and the compositor would never take the repaint's
  // commit. Releasing the calls in flight lets it finish the draw it was in
  // and get to this one; whether that earlier draw can end up answering this
  // repaint is the question at gRepaintEvent below.
  recordreplay::SignalInFlightSyncCalls();

  gCurrentCompositorProxy = RepaintCompositorProxy();
  if (!gCurrentCompositorProxy)
    return nullptr;

  if (gCompositorProxies &&
      (!gCurrentCompositorProxy->RecordReplayHasRootLayer() ||
       !gCurrentCompositorProxy->RecordReplayIsVisible())) {
    // Says what there was to choose from when the page chosen can't be drawn
    // as it is.
    const base::UnguessableToken* embed_token = gSurfaceEmbedToken;
    size_t index = 0;
    for (cc::ProxyMain* proxy : *gCompositorProxies) {
      Print("Repaint: compositor %zu of %zu%s: layers %d, visible %d, owns the "
            "paint surface %d (a surface is owned %d).",
            ++index, gCompositorProxies->size(),
            proxy == gCurrentCompositorProxy ? " (chosen)" : "",
            proxy->RecordReplayHasRootLayer(), proxy->RecordReplayIsVisible(),
            embed_token &&
                proxy->RecordReplayLocalSurfaceId().embed_token() ==
                    *embed_token,
            !!embed_token);
    }
  }

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
  gRepaintFramesSubmitted = 0;
  gRepaintFramesForOtherSurface = 0;
  gRepaintFramesPainted = 0;
  // From here a draw that finishes encodes its frame as this repaint's result
  // (OnPaintFinished), so what keeps an *earlier* draw from answering matters.
  // Usually the commit above does: BeginMainFrameWithBlocking(..., true)
  // returns only once the impl thread has run NotifyReadyToCommitOnImpl, which
  // it cannot reach until the draw it was inside has returned.
  //
  // It does not hold when the commit is *skipped*: force_blocking sets
  // skip_commit = !root_layer() and that branch returns without waiting, so
  // for a page with no layers a draw already in flight can be the result.
  // Answering with the tree the compositor last drew is that path's intent,
  // but if no window owns the paint surface the frame can be of a window this
  // repaint did not pick. Closing that needs a blocking round trip to the impl
  // thread before this line; no case in the suite pauses on a layerless page.
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
  if (!gRepaintResult) {
    Print("[RuntimeError] Repaint drew nothing: %d frames submitted, %d for "
          "another surface, %d painted. The page %s layers and %s visible. "
          "%zu compositors in this process.",
          gRepaintFramesSubmitted.load(), gRepaintFramesForOtherSurface.load(),
          gRepaintFramesPainted.load(),
          gCurrentCompositorProxy->RecordReplayHasRootLayer() ? "has" : "has no",
          gCurrentCompositorProxy->RecordReplayIsVisible() ? "is" : "isn't",
          gCompositorProxies ? gCompositorProxies->size() : 0);
    // The compositor thread says what it's waiting on, and gets a moment to.
    gCurrentCompositorProxy->RecordReplayDescribeRepaint();
    base::PlatformThread::Sleep(base::Milliseconds(200));
  }
  return gRepaintResult;
}

gfx::Size GetCurrentViewportPixelSizeImpl() {
  return gDeviceViewportSize;
}

} // namespace recordreplay
