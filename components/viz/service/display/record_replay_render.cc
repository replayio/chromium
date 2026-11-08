// Copyright 2020 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display/record_replay_render.h"

#include <deque>
#include <vector>

#include "base/base64.h"
#include "base/containers/contains.h"
#include "base/metrics/record_histogram_checker.h"
#include "base/metrics/statistics_recorder.h"
#include "base/no_destructor.h"
#include "base/record_replay.h"
#include "base/record_replay_paint_surface.h"
#include "base/record_replay_render_interface.h"
#include "base/strings/stringprintf.h"
#include "cc/animation/animation_events.h"
#include "cc/paint/filter_operations.h"
#include "cc/paint/image_provider.h"
#include "cc/paint/paint_filter.h"
#include "cc/paint/record_replay_image_capture.h"
#include "cc/trees/compositor_commit_data.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/quads/picture_draw_quad.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display/software_renderer.h"
#include "components/viz/service/display_embedder/software_output_surface.h"
#include "third_party/skia/include/private/SkRecordReplayDraw.h"
#include "ui/gfx/codec/jpeg_codec.h"

namespace blink {
extern bool RecordReplayStateEnsureInitialized();
}

namespace recordreplay {

extern bool IsMainThread();

struct SharedBitmapInfo {
  viz::SharedBitmapId id_;
  uint8_t* memory_;
  size_t size_;
};
typedef std::vector<SharedBitmapInfo> SharedBitmapInfoVector;
static SharedBitmapInfoVector* gSharedBitmaps;

void NotifyRasterBuffer(const viz::SharedBitmapId& shared_bitmap_id,
                        void* memory, size_t size) {
  if (!gSharedBitmaps) {
    gSharedBitmaps = new SharedBitmapInfoVector();
  }
  gSharedBitmaps->push_back({ shared_bitmap_id, (uint8_t*)memory, size });
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

// Code that copies, captures or draws frames in-process runs with recorded
// events disallowed and in an isolated Skia draw, which takes IDs from ranges
// of its own and uses caches of its own.
class ScopedIsolatedDraw {
 public:
  explicit ScopedIsolatedDraw(const char* why) : disallow_(why) {
    SkRecordReplayBeginIsolatedDraw();
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
    SetInIsolatedDrawForChecks(true);
#endif
  }
  ~ScopedIsolatedDraw() {
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
    SetInIsolatedDrawForChecks(false);
#endif
    SkRecordReplayEndIsolatedDraw();
  }

 private:
  AutoDisallowEvents disallow_;
};

// Histograms are shared with recorded threads: their samples are sent to the
// browser and observers of them post tasks. A histogram first used in an
// in-process draw, which only happens when replaying, is a dummy. A histogram
// used before still gets the draw's samples, from macros that cached it too,
// which this doesn't see; the code that draws records none outside the draw.
class RecordReplayDrawHistogramChecker : public base::RecordHistogramChecker {
 public:
  bool ShouldRecord(uint32_t histogram_hash) const override {
    return !SkRecordReplayInIsolatedDraw();
  }
};

static void InitializeRenderer() {
  static bool installed_histogram_checker = false;
  if (!installed_histogram_checker) {
    installed_histogram_checker = true;
    base::StatisticsRecorder::SetRecordChecker(
        std::make_unique<RecordReplayDrawHistogramChecker>());
  }
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

// What drawing a submitted frame needs: the frame itself is sent to the GPU
// process, which destroys it, before its paints finish.
struct RecordReplayFrame {
  viz::AggregatedRenderPassList render_passes;
  std::vector<viz::TransferableResource> resources;
  gfx::Size size_in_pixels;
};

static std::unique_ptr<RecordReplayFrame> CopyFrame(
    const viz::CompositorFrame& frame) {
  auto copy = std::make_unique<RecordReplayFrame>();
  for (const auto& pass : frame.render_pass_list) {
    copy->render_passes.push_back(pass->DeepCopyAggregated());
  }
  copy->resources = frame.resource_list;
  copy->size_in_pixels = frame.size_in_pixels();
  return copy;
}

// Resources of the frame being drawn.
static const std::vector<viz::TransferableResource>* gCurrentResources;

// When replaying, a copy of the submitted frame whose paints finish next.
static base::NoDestructor<std::unique_ptr<RecordReplayFrame>> gSubmittedFrame;

// Whether a frame was submitted whose paints haven't finished.
static bool gSubmittedFramePending;

// References the submitted frame's copy shares with the frame: its filters.
// They are held, recording and replaying, until the frame's paints finish and
// the copy is gone, and dropped with events allowed: the copy never holds the
// last reference, whose release runs destructors that can post Skia cache
// purges to recorded threads. Filters are thus destroyed after the frame is
// sent, in both modes. Frames this renderer draws in software have no picture
// quads (PictureLayerImpl appends them only in resourceless software draws),
// so they share no display lists with the copy.
struct RecordReplayFrameRefs {
  std::vector<cc::FilterOperations> filters;
};
static base::NoDestructor<RecordReplayFrameRefs> gSubmittedFrameRefs;

static void HoldFrameRefs(const viz::CompositorFrame& frame) {
  for (const auto& pass : frame.render_pass_list) {
#if defined(RECORD_REPLAY_ISOLATED_DRAW_CHECKS)
    for (const viz::DrawQuad* quad : pass->quad_list) {
      if (quad->material == viz::DrawQuad::Material::kPictureContent) {
        IsolatedDrawCheckAlways("picture quad", quad);
      }
    }
#endif
    if (!pass->filters.IsEmpty()) {
      gSubmittedFrameRefs->filters.push_back(pass->filters);
    }
    if (!pass->backdrop_filters.IsEmpty()) {
      gSubmittedFrameRefs->filters.push_back(pass->backdrop_filters);
    }
  }
}

extern const SkPixmap* gCurrentPixmap;
static void FinishPaints();
static void SetDeviceViewportSize(const gfx::Size& size);

// What a frame's draw does depends on its contents, and so do the recorded
// events it emits (asserts, recorded IDs, ordered locks). Without diverging, a
// submitted frame is therefore drawn only when the recorder asks for its
// pixels, with recorded events disallowed, so that drawing a frame or not
// never changes the recording's events.
static bool RecordReplayDrawWhenRequested() {
  static bool enabled = IsRecordingOrReplaying("event-free-draw");
  return enabled && !HasDivergedFromRecording();
}

// Whether the current frame has been drawn into |gCurrentPixmap|.
static bool gCurrentFrameDrawn;

// Hands a draw the images the frame's send to the GPU process serialized
// (cc::RecordReplayCaptureImage), and nothing for others, which drops their
// filters: decoding images would fill caches that recorded threads share.
class RecordReplayCapturedImageProvider : public cc::ImageProvider {
 public:
  ScopedResult GetRasterContent(const cc::DrawImage& draw_image) override {
    sk_sp<SkImage> image =
        cc::RecordReplayFindCapturedImage(draw_image.paint_image());
    if (!image) {
      return ScopedResult();
    }
    return ScopedResult(cc::DecodedDrawImage(
        std::move(image), nullptr, SkSize::Make(0, 0), SkSize::Make(1, 1),
        draw_image.filter_quality(), /*is_budgeted=*/true));
  }

  bool RecordReplaySnapshotsAllFilters() const override { return true; }
};

static bool HasReferenceFilter(const cc::FilterOperations& filters) {
  for (const cc::FilterOperation& op : filters.operations()) {
    if (op.type() == cc::FilterOperation::REFERENCE && op.image_filter()) {
      return true;
    }
  }
  return false;
}

static cc::FilterOperations SnapshotFilters(
    const cc::FilterOperations& filters,
    cc::ImageProvider* image_provider) {
  std::vector<cc::FilterOperation> snapshot;
  for (const cc::FilterOperation& op : filters.operations()) {
    if (op.type() == cc::FilterOperation::REFERENCE && op.image_filter()) {
      snapshot.push_back(cc::FilterOperation::CreateReferenceFilter(
          op.image_filter()->SnapshotWithImages(image_provider)));
    } else {
      snapshot.push_back(op);
    }
  }
  return cc::FilterOperations(std::move(snapshot));
}

// Drawing a reference filter can decode images, play back paint records and
// fill caches that recorded threads share. A draw uses snapshots of the
// frame's reference filters instead, with the images the send captured and
// paint records emptied, as viz receives them.
static void SnapshotReferenceFilters(RecordReplayFrame& frame) {
  RecordReplayCapturedImageProvider image_provider;
  for (auto& pass : frame.render_passes) {
    if (HasReferenceFilter(pass->filters)) {
      pass->filters = SnapshotFilters(pass->filters, &image_provider);
    }
    if (HasReferenceFilter(pass->backdrop_filters)) {
      pass->backdrop_filters =
          SnapshotFilters(pass->backdrop_filters, &image_provider);
    }
  }
}

static bool HasReferenceFilters(const viz::CompositorFrame& frame) {
  for (const auto& pass : frame.render_pass_list) {
    if (HasReferenceFilter(pass->filters) ||
        HasReferenceFilter(pass->backdrop_filters)) {
      return true;
    }
  }
  return false;
}

static void DrawFrame(RecordReplayFrame& frame) {
  // A frame drawn when its pixels are requested stays current until its
  // paints have finished.
  const std::vector<viz::TransferableResource>* previous_resources =
      gCurrentResources;
  gCurrentResources = &frame.resources;

  gRenderer->DrawFrame(&frame.render_passes,
                       1,
                       frame.size_in_pixels,
                       gfx::DisplayColorSpaces(),
                       viz::SurfaceDamageRectList());

  gCurrentResources = previous_resources;
}

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

  if (RecordReplayDrawWhenRequested()) {
    // The frame's paints finish once it has been sent to the GPU process
    // (FinishSubmittedCompositorFrame), when the paint callback draws a copy
    // of it if it's asked for the frame's pixels. Only replaying draws.
    gSubmittedFramePending = true;
    SetDeviceViewportSize(frame.size_in_pixels());
    HoldFrameRefs(frame);
    if (IsReplaying()) {
      // Every frame is copied: sending it to the GPU process destroys it
      // before its paints finish, when it's known whether they need pixels.
      ScopedIsolatedDraw isolated("RecordReplayCopySubmittedFrame");
      *gSubmittedFrame = CopyFrame(frame);
      cc::RecordReplaySetCapturingImages(HasReferenceFilters(frame));
    }
    return;
  }

  std::unique_ptr<RecordReplayFrame> copy = CopyFrame(frame);
  DrawFrame(*copy);
}

void FinishSubmittedCompositorFrame() {
  if (!gSubmittedFramePending) {
    return;
  }
  gSubmittedFramePending = false;
  cc::RecordReplaySetCapturingImages(false);

  gCurrentFrameDrawn = false;
  FinishPaints();
  gCurrentPixmap = nullptr;

  if (*gSubmittedFrame) {
    ScopedIsolatedDraw isolated("RecordReplayCopySubmittedFrame");
    gSubmittedFrame->reset();
    cc::RecordReplayClearCapturedImages();
  }
  gSubmittedFrameRefs->filters.clear();
}

bool PopulateSkBitmapWithResource(SkBitmap* sk_bitmap, viz::ResourceId resource_id) {
  CHECK(gCurrentResources);

  const viz::TransferableResource* transferable = nullptr;
  for (const viz::TransferableResource& resource : *gCurrentResources) {
    if (resource.id == resource_id) {
      transferable = &resource;
      break;
    }
  }

  recordreplay::AssertMaybeEventsDisallowed("[RUN-2847-3001] recordreplay::PopulateSkBitmapWithResource A %d", !!transferable);

  if (!transferable) {
    return false;
  }

  void* pixels = nullptr;
  if (gSharedBitmaps) {
    for (const SharedBitmapInfo& info : *gSharedBitmaps) {
      if (info.id_ == transferable->mailbox_holder.mailbox) {
        pixels = info.memory_;
      }
    }
  }

  recordreplay::AssertMaybeEventsDisallowed("[RUN-2847-3001] recordreplay::PopulateSkBitmapWithResource B %d", !!pixels);
  if (!pixels) {
    return false;
  }

  SkImageInfo info =
      SkImageInfo::MakeN32Premul(transferable->size.width(),
                                 transferable->size.height());
  return sk_bitmap->installPixels(info, pixels, info.minRowBytes());
}

extern "C" size_t V8RecordReplayPaintStart();
extern "C" void V8RecordReplayPaintFinished(size_t bookmark);
extern "C" void V8RecordReplaySetPaintCallback(char* (*callback)(const char*, int));

const SkPixmap* gCurrentPixmap;

static char* EncodeBitmapContents(const char* mime_type, int jpeg_quality) {
  if (!gCurrentPixmap) {
    return nullptr;
  }

  if (strcmp(mime_type, "image/jpeg")) {
    // NYI
    return nullptr;
  }

  std::vector<unsigned char> data;
  if (!gfx::JPEGCodec::Encode(*gCurrentPixmap, jpeg_quality,
                              SkJpegEncoder::Downsample::k444, &data)) {
    Print("Error: JPEG encoding failed");
    return nullptr;
  }

  std::string encoded = base::Base64Encode(data);
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

  if (RecordReplayDrawWhenRequested()) {
    if (!*gSubmittedFrame) {
      return nullptr;
    }
    if (!gCurrentFrameDrawn) {
      gCurrentFrameDrawn = true;
      ScopedIsolatedDraw isolated("RecordReplayDrawWhenRequested");
      SnapshotReferenceFilters(**gSubmittedFrame);
      DrawFrame(**gSubmittedFrame);
    }
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
// Frames drawn before the commit's tree activates still show earlier commits,
// so a commit's bookmark is finished by the first frame its compositor draws
// from a tree with the commit's source frame number or a later one.

// Bookmark which was last created on the main thread before blocking until the
// compositor thread is ready to commit.
static std::atomic<size_t> gCurrentPaintBookmark;

// Bookmark for the last point where a paint was committed on the main thread,
// once a frame draws that commit's tree.
static std::atomic<size_t> gLastCommitBookmark;

// Commits whose trees no frame has drawn yet, oldest first. Source frame
// numbers count per compositor. Only used on the compositor thread.
struct PendingCommitPaint {
  const void* compositor;
  int source_frame_number;
  size_t bookmark;
};
static base::NoDestructor<std::deque<PendingCommitPaint>> gPendingCommitPaints;

// Bookmarks of commits whose trees were replaced before any frame drew them,
// finished along with the next paint.
static base::NoDestructor<std::vector<size_t>> gSupersededCommitBookmarks;

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

void OnReadyToCommit(const void* compositor, int source_frame_number) {
  size_t bookmark = gCurrentPaintBookmark;
  if (bookmark) {
    gPendingCommitPaints->push_back({compositor, source_frame_number, bookmark});
  }
}

void OnDrawSourceFrame(const void* compositor, int source_frame_number) {
  size_t bookmark = 0;
  for (auto it = gPendingCommitPaints->begin(); it != gPendingCommitPaints->end();) {
    if (it->compositor != compositor ||
        it->source_frame_number > source_frame_number) {
      ++it;
      continue;
    }
    if (bookmark) {
      gSupersededCommitBookmarks->push_back(bookmark);
    }
    bookmark = it->bookmark;
    it = gPendingCommitPaints->erase(it);
  }
  if (bookmark) {
    gLastCommitBookmark.store(bookmark, std::memory_order_relaxed);
  }
}

// A destroyed compositor never draws its pending commits, and a later
// compositor at the same address must not finish them.
void OnCompositorDestroyed(const void* compositor) {
  for (auto it = gPendingCommitPaints->begin(); it != gPendingCommitPaints->end();) {
    if (it->compositor != compositor) {
      ++it;
      continue;
    }
    gSupersededCommitBookmarks->push_back(it->bookmark);
    it = gPendingCommitPaints->erase(it);
  }
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

static void SetDeviceViewportSize(const gfx::Size& size) {
  gDeviceViewportSize = size;
}

static void FinishPaints() {
  for (size_t superseded : *gSupersededCommitBookmarks) {
    V8RecordReplayPaintFinished(superseded);
  }
  gSupersededCommitBookmarks->clear();
  size_t bookmark = gLastCommitBookmark.load();
  if (bookmark) {
    V8RecordReplayPaintFinished(bookmark);
  }
}

// The pixels of the frame the paint callback draws, which it encodes after
// the draw.
static base::NoDestructor<SkPixmap> gDrawnPixmap;

void OnPaintFinished(const SkPixmap& pixmap) {
  if (RecordReplayDrawWhenRequested()) {
    // The paint callback is drawing the current frame.
    CHECK(gCurrentResources && AreEventsDisallowed());
    *gDrawnPixmap = pixmap;
    gCurrentPixmap = gDrawnPixmap.get();
    return;
  }

  gCurrentPixmap = &pixmap;
  if (gOutputSurface) {
    // Obtain device size on impl thread.
    SetDeviceViewportSize(
        gOutputSurface->software_device()->ReplayViewportPixelSize());
  }

  if (HasDivergedFromRecording()) {
    // If we've diverged from the recording then we're probably repainting,
    // and in any case don't need to notify the recorder that the paint finished.
    if (gRepaintEvent)
      gRepaintResult = EncodeBitmapContents(gRepaintMimeType, gRepaintJPEGQuality);
  } else {
    FinishPaints();
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
    if (!base::Contains(*gCompositorProxies, proxy)) {
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
  if (embed_token && gCompositorProxies) {
    for (cc::ProxyMain* proxy : *gCompositorProxies) {
      if (proxy->RecordReplayLocalSurfaceId().embed_token() == *embed_token) {
        return proxy;
      }
    }
  }
  return gCurrentCompositorProxy;
}

static char* PaintWhenDiverged(const char* mime_type, int jpeg_quality) {
  CHECK(IsMainThread());

  gCurrentCompositorProxy = RepaintCompositorProxy();
  if (!gCurrentCompositorProxy)
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
