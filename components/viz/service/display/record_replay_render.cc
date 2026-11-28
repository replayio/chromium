// Copyright 2020 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display/record_replay_render.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/metrics/record_histogram_checker.h"
#include "base/metrics/statistics_recorder.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "base/record_replay.h"
#include "base/record_replay_isolated_draw_asserts.h"
#include "base/record_replay_paint_surface.h"
#include "base/record_replay_render_interface.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "cc/animation/animation_events.h"
#include "cc/paint/filter_operations.h"
#include "cc/paint/image_provider.h"
#include "cc/paint/paint_filter.h"
#include "cc/paint/record_replay_image_capture.h"
#include "cc/paint/record_replay_isolated_draw.h"
#include "cc/trees/compositor_commit_data.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/quads/aggregated_render_pass_draw_quad.h"
#include "components/viz/common/quads/picture_draw_quad.h"
#include "components/viz/common/resources/shared_image_format_utils.h"
#include "components/viz/common/resources/transferable_resource.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display/software_renderer.h"
#include "components/viz/service/display_embedder/software_output_surface.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "third_party/skia/include/private/SkRecordReplayDraw.h"
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

// What a repaint's draw got as far as, said when it comes back with nothing.
static std::atomic<int> gRepaintFramesSubmitted;
static std::atomic<int> gRepaintFramesForOtherSurface;
static std::atomic<int> gRepaintFramesPainted;

// What drawing a submitted frame needs: the frame itself is sent to the GPU
// process, which destroys it, before its paints finish. The resources hold
// the frame's tiles, whose pixels stay as they are until the GPU process
// returns them.
struct RecordReplayFrame {
  viz::AggregatedRenderPassList render_passes;
  std::vector<viz::TransferableResource> resources;
  gfx::Size size_in_pixels;
};

static std::unique_ptr<RecordReplayFrame> CopyFrame(
    const viz::CompositorFrame& frame) {
  auto copy = std::make_unique<RecordReplayFrame>();
  for (const auto& pass : frame.render_pass_list) {
    copy->render_passes.push_back(pass->DeepCopyAggregated(frame.render_pass_list));
  }
  copy->resources = frame.resource_list;
  copy->size_in_pixels = frame.size_in_pixels();
  return copy;
}

// Resources of the frame being drawn, and the mappings of its tiles that are
// in use while it's drawn.
static const std::vector<viz::TransferableResource>* gCurrentResources;
static std::vector<std::unique_ptr<gpu::ClientSharedImage::ScopedMapping>>*
    gCurrentMappings;

// When replaying, a copy of the submitted frame whose paints finish next.
static std::unique_ptr<RecordReplayFrame>& SubmittedFrame() {
  static base::NoDestructor<std::unique_ptr<RecordReplayFrame>> frame;
  return *frame;
}

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
static RecordReplayFrameRefs& SubmittedFrameRefs() {
  static base::NoDestructor<RecordReplayFrameRefs> refs;
  return *refs;
}

static void HoldFrameRefs(const viz::CompositorFrame& frame) {
  for (const auto& pass : frame.render_pass_list) {
    if (IsolatedDrawAssertsEnabled()) {
      for (const viz::DrawQuad* quad : pass->quad_list) {
        if (quad->material == viz::DrawQuad::Material::kPictureContent) {
          IsolatedDrawNote("picture quad", quad);
        }
      }
    }
    if (!pass->filters.IsEmpty()) {
      SubmittedFrameRefs().filters.push_back(pass->filters);
    }
    if (!pass->backdrop_filters.IsEmpty()) {
      SubmittedFrameRefs().filters.push_back(pass->backdrop_filters);
    }
  }
}

const SkPixmap* gCurrentPixmap;
static void FinishPaints();
static void FinishIgnoredFramePaint();

// The compositor drawing a frame (OnDrawSourceFrame), and the one whose frame
// was submitted last. Only used on the compositor thread.
static const void* gDrawingCompositor;
static const void* gSubmittingCompositor;

// Whether a paint is being finished unchanged (FinishPaintUnchanged).
static bool gFinishingUnchanged;
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
          cc::RecordReplayIsUnsentFilter(op.image_filter().get())
              ? nullptr
              : op.image_filter()->SnapshotWithImages(image_provider)));
    } else {
      snapshot.push_back(op);
    }
  }
  return cc::FilterOperations(std::move(snapshot));
}

// Drawing a reference filter can decode images, play back paint records and
// fill caches that recorded threads share. A draw uses snapshots of the
// frame's reference filters instead, with the images the send captured,
// paint records emptied and no filter where the send couldn't serialize one,
// as viz receives them.
static void SnapshotReferenceFilters(RecordReplayFrame& frame) {
  RecordReplayCapturedImageProvider image_provider;
  // An aggregated pass's filters are on the quads that draw it.
  for (auto& pass : frame.render_passes) {
    for (viz::DrawQuad* quad : pass->quad_list) {
      if (quad->material != viz::DrawQuad::Material::kAggregatedRenderPass) {
        continue;
      }
      auto* pass_quad = static_cast<viz::AggregatedRenderPassDrawQuad*>(quad);
      if (HasReferenceFilter(pass_quad->filters)) {
        pass_quad->filters =
            SnapshotFilters(pass_quad->filters, &image_provider);
      }
      if (HasReferenceFilter(pass_quad->backdrop_filters)) {
        pass_quad->backdrop_filters =
            SnapshotFilters(pass_quad->backdrop_filters, &image_provider);
      }
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
  auto* previous_mappings = gCurrentMappings;
  std::vector<std::unique_ptr<gpu::ClientSharedImage::ScopedMapping>> mappings;
  gCurrentResources = &frame.resources;
  gCurrentMappings = &mappings;

  gRenderer->DrawFrame(&frame.render_passes,
                       1,
                       frame.size_in_pixels,
                       gfx::DisplayColorSpaces(),
                       viz::SurfaceDamageRectList(),
                       viz::TrackedElementRects());

  gCurrentResources = previous_resources;
  gCurrentMappings = previous_mappings;
}

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
    FinishIgnoredFramePaint();
    return;
  }
  gSubmittingCompositor = gDrawingCompositor;

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
      cc::RecordReplayScopedIsolatedDraw isolated("RecordReplayCopySubmittedFrame");
      SubmittedFrame() = CopyFrame(frame);
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

  if (SubmittedFrame()) {
    cc::RecordReplayScopedIsolatedDraw isolated("RecordReplayCopySubmittedFrame");
    SubmittedFrame().reset();
    cc::RecordReplayClearCapturedImages();
  }
  SubmittedFrameRefs().filters.clear();
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

static char* EncodeBitmapContents(const char* mime_type, int jpeg_quality) {
  if (!gCurrentPixmap) {
    return nullptr;
  }

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
    // Paints the compositor thread finishes while diverged, such as a
    // repaint's frames, aren't the recording's.
    if (!IsMainThread()) {
      return nullptr;
    }
    return PaintWhenDiverged(mime_type, jpeg_quality);
  }

  if (gFinishingUnchanged) {
    // An empty result tells the linker that the paint was finished unchanged
    // and shows the paint before it, unlike no result, which means its pixels
    // aren't available. It is freed like any other result.
    return strdup("");
  }

  if (RecordReplayDrawWhenRequested()) {
    if (!SubmittedFrame()) {
      return nullptr;
    }
    cc::RecordReplayScopedIsolatedDraw isolated("RecordReplayDrawWhenRequested");
    if (!gCurrentFrameDrawn) {
      gCurrentFrameDrawn = true;
      SnapshotReferenceFilters(*SubmittedFrame());
      DrawFrame(*SubmittedFrame());
    }
    return EncodeBitmapContents(mime_type, jpeg_quality);
  }

  return EncodeBitmapContents(mime_type, jpeg_quality);
}

// Paints generally occur in the following way:
//
// 1. Main thread commits a paint in ProxyMain::BeginMainFrame and posts a task
//    to run ProxyImpl::NotifyReadyToCommitOnImpl on the compositor thread. It
//    only waits for that task when the commit blocks, as a repaint's does.
//
// 2. Compositor thread runs ProxyImpl::NotifyReadyToCommitOnImpl, and later
//    commits the tree and activates it.
//
// 3. Compositor thread finishes painting the frame.
//
// The paint finished in #3 needs to be associated with the bookmark created in
// #1, which the commit carries (CommitState::record_replay_paint_bookmark): the
// main thread doesn't wait for the compositor thread to take a commit, so the
// next main frame, of this compositor or another, can start its own paint
// first. Frames drawn before the commit's tree activates still show earlier
// commits, so a commit's bookmark is finished by the first frame its
// compositor draws from a tree with the commit's source frame number or a later
// one.

// Bookmark for the last point where a paint was committed on the main thread,
// once a frame draws that commit's tree. Only used on the compositor thread.
static size_t gLastCommitBookmark;

// Whether |gLastCommitBookmark|'s paint was finished, by a sent frame, or
// unchanged when its frame was ignored or its compositor destroyed, and
// the compositor whose frame drew its commit. Only used on the compositor
// thread.
static bool gLastCommitBookmarkFinished = true;
static const void* gLastCommitCompositor;

// Commits whose trees no frame has drawn yet, oldest first. Source frame
// numbers count per compositor. Only used on the compositor thread.
struct PendingCommitPaint {
  // Only compared, never dereferenced.
  RAW_PTR_EXCLUSION const void* compositor;
  int source_frame_number;
  size_t bookmark;
};
static std::deque<PendingCommitPaint>& PendingCommitPaints() {
  static base::NoDestructor<std::deque<PendingCommitPaint>> paints;
  return *paints;
}

// Paints of commits whose trees no sent frame showed: replaced before any
// frame drew them, drawn by frames that weren't sent (an aborted draw, a
// surface that isn't drawn) or left by a destroyed compositor. They are
// finished unchanged once that's known, so that they show the paint
// before them, which the screen still showed, and aren't among the started
// paints the linker reports pending at the end of a paints run.
static void FinishPaintUnchanged(size_t bookmark) {
  gFinishingUnchanged = true;
  V8RecordReplayPaintFinished(bookmark);
  gFinishingUnchanged = false;
}

void InitPaintCallback() {
  static bool hasPaints = false;
  if (!hasPaints) {
    hasPaints = true;
    V8RecordReplaySetPaintCallback(PaintCallback);
  }
}

size_t OnCommitPaint() {
  // Record/replay state has to be initialized before the first paint
  // starts, as a checkpoint must have been taken.
  if (blink::RecordReplayStateEnsureInitialized()) {
    return V8RecordReplayPaintStart();
  }
  return 0;
}

void OnReadyToCommit(const void* compositor,
                     int source_frame_number,
                     size_t bookmark) {
  if (bookmark) {
    PendingCommitPaints().push_back({compositor, source_frame_number, bookmark});
  }
}

void OnDrawSourceFrame(const void* compositor, int source_frame_number) {
  gDrawingCompositor = compositor;
  size_t bookmark = 0;
  for (auto it = PendingCommitPaints().begin(); it != PendingCommitPaints().end();) {
    if (it->compositor != compositor ||
        it->source_frame_number > source_frame_number) {
      ++it;
      continue;
    }
    if (bookmark) {
      FinishPaintUnchanged(bookmark);
    }
    bookmark = it->bookmark;
    it = PendingCommitPaints().erase(it);
  }
  if (bookmark) {
    size_t last = gLastCommitBookmark;
    if (last && !gLastCommitBookmarkFinished) {
      IsolatedDrawNote("paint bookmark without a sent frame", compositor);
      FinishPaintUnchanged(last);
    }
    gLastCommitBookmark = bookmark;
    gLastCommitBookmarkFinished = false;
    gLastCommitCompositor = compositor;
  }
}

// A destroyed compositor never draws its pending commits, and a later
// compositor at the same address must not finish them.
void OnCompositorDestroyed(const void* compositor) {
  for (auto it = PendingCommitPaints().begin(); it != PendingCommitPaints().end();) {
    if (it->compositor != compositor) {
      ++it;
      continue;
    }
    FinishPaintUnchanged(it->bookmark);
    it = PendingCommitPaints().erase(it);
  }
  // Nor is its last drawn commit's paint finished by another compositor's
  // frames. This is bookkeeping: no page was found where that paint showed the
  // next compositor's pixels instead of the earlier paint's.
  if (gLastCommitCompositor == compositor) {
    size_t last = gLastCommitBookmark;
    if (last && !gLastCommitBookmarkFinished) {
      IsolatedDrawNote("paint bookmark of a destroyed compositor", compositor);
      FinishPaintUnchanged(last);
    }
    gLastCommitBookmark = 0;
    gLastCommitBookmarkFinished = true;
    gLastCommitCompositor = nullptr;
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

// A commit drawn by a frame of a surface that isn't drawn was never on screen,
// so its paint is finished unchanged when that frame is ignored.
static void FinishIgnoredFramePaint() {
  size_t last = gLastCommitBookmark;
  if (last && !gLastCommitBookmarkFinished &&
      gLastCommitCompositor == gDrawingCompositor) {
    FinishPaintUnchanged(last);
    gLastCommitBookmarkFinished = true;
  }
}

static void FinishPaints() {
  // Only the frames of the compositor that drew the commit finish its paint.
  // Another compositor's drawn commit was finished unchanged when its
  // frame was ignored, or, if its draw sent no frame, is when any compositor
  // draws its next commit or that compositor is destroyed; until then it stays
  // pending, also through frames of other compositors without a new commit.
  // Each paint is finished once, by the first sent frame.
  size_t bookmark = gLastCommitBookmark;
  if (bookmark && !gLastCommitBookmarkFinished &&
      gLastCommitCompositor == gSubmittingCompositor) {
    V8RecordReplayPaintFinished(bookmark);
    gLastCommitBookmarkFinished = true;
  }
}

// The pixels of the frame the paint callback draws, which it encodes after
// the draw.
static SkPixmap& DrawnPixmap() {
  static base::NoDestructor<SkPixmap> pixmap;
  return *pixmap;
}

void OnPaintFinished(const SkPixmap& pixmap) {
  if (RecordReplayDrawWhenRequested()) {
    // The paint callback is drawing the current frame.
    CHECK(gCurrentResources && SkRecordReplayInIsolatedDraw());
    DrawnPixmap() = pixmap;
    gCurrentPixmap = &DrawnPixmap();
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
    gRepaintFramesPainted++;
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

  // This repaint needs the compositor sequence to run the commit below, and
  // that sequence can be blocked inside a synchronous mojo call it entered
  // before this child diverged: a GPU channel flush for the sync tokens of the
  // resources it was drawing with (GpuChannelHost::VerifyFlush). A diverged
  // child gets no replies, so the call would never return, the sequence would
  // run no further task, and the commit would never be taken. Releasing the
  // calls in flight lets it finish that draw and reach this one. Whether the
  // earlier draw can end up answering this repaint is the question at
  // gRepaintEvent below.
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
  // (OnPaintFinished), so what keeps an earlier draw from answering matters.
  // Usually the commit above does: BeginMainFrameWithBlocking(..., true)
  // returns only once the impl thread has run NotifyReadyToCommitOnImpl, which
  // it cannot reach until the draw it was inside has returned.
  //
  // It does not hold when the commit is skipped: force_blocking sets
  // skip_commit = !root_layer() and that branch returns without waiting, so
  // for a page with no layers a draw already in flight can be the result.
  // Answering with the tree the compositor last drew is that path's intent,
  // but if no window owns the paint surface the frame can be of a window this
  // repaint did not pick. Closing that would need a blocking round trip to the
  // impl thread before this line.
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
