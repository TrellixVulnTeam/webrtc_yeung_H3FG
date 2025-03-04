/*
 * Copyright (C) 2006 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2008, 2009 Apple Inc. All rights reserved.
 * Copyright (C) Research In Motion Limited 2011. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/svg/graphics/SVGImage.h"

#include "core/animation/DocumentAnimations.h"
#include "core/animation/DocumentTimeline.h"
#include "core/dom/FlatTreeTraversal.h"
#include "core/dom/NodeTraversal.h"
#include "core/dom/TaskRunnerHelper.h"
#include "core/frame/LocalFrame.h"
#include "core/frame/LocalFrameClient.h"
#include "core/frame/LocalFrameView.h"
#include "core/frame/Settings.h"
#include "core/layout/LayoutView.h"
#include "core/layout/svg/LayoutSVGRoot.h"
#include "core/loader/FrameLoadRequest.h"
#include "core/paint/FloatClipRecorder.h"
#include "core/paint/PaintLayer.h"
#include "core/paint/TransformRecorder.h"
#include "core/style/ComputedStyle.h"
#include "core/svg/SVGDocumentExtensions.h"
#include "core/svg/SVGFEImageElement.h"
#include "core/svg/SVGImageElement.h"
#include "core/svg/SVGSVGElement.h"
#include "core/svg/animation/SMILTimeContainer.h"
#include "core/svg/graphics/SVGImageChromeClient.h"
#include "platform/EventDispatchForbiddenScope.h"
#include "platform/LengthFunctions.h"
#include "platform/ScriptForbiddenScope.h"
#include "platform/geometry/IntRect.h"
#include "platform/graphics/Color.h"
#include "platform/graphics/GraphicsContext.h"
#include "platform/graphics/ImageBuffer.h"
#include "platform/graphics/ImageObserver.h"
#include "platform/graphics/paint/ClipRecorder.h"
#include "platform/graphics/paint/CullRect.h"
#include "platform/graphics/paint/DrawingRecorder.h"
#include "platform/graphics/paint/PaintRecord.h"
#include "platform/graphics/paint/PaintRecordBuilder.h"
#include "platform/instrumentation/tracing/TraceEvent.h"
#include "platform/wtf/RefPtr.h"

namespace blink {

// SVGImageLocalFrameClient is used to wait until SVG document's load event
// in the case where there are subresources asynchronously loaded.
//
// Reference cycle: SVGImage -(Persistent)-> Page -(Member)-> Frame -(Member)->
// FrameClient == SVGImageLocalFrameClient -(raw)-> SVGImage.
class SVGImage::SVGImageLocalFrameClient : public EmptyLocalFrameClient {
 public:
  SVGImageLocalFrameClient(SVGImage* image) : image_(image) {}

  void ClearImage() { image_ = nullptr; }

 private:
  void DispatchDidHandleOnloadEvents() override {
    // The SVGImage was destructed before SVG load completion.
    if (!image_)
      return;

    image_->LoadCompleted();
  }

  // Cleared manually by SVGImage's destructor when |image_| is destructed.
  SVGImage* image_;
};

SVGImage::SVGImage(ImageObserver* observer, bool is_multipart)
    : Image(observer, is_multipart),
      paint_controller_(PaintController::Create()),
      has_pending_timeline_rewind_(false) {}

SVGImage::~SVGImage() {
  if (frame_client_)
    frame_client_->ClearImage();

  if (page_) {
    // Store m_page in a local variable, clearing m_page, so that
    // SVGImageChromeClient knows we're destructed.
    Page* current_page = page_.Release();
    // Break both the loader and view references to the frame
    current_page->WillBeDestroyed();
  }

  // Verify that page teardown destroyed the Chrome
  DCHECK(!chrome_client_ || !chrome_client_->GetImage());
}

bool SVGImage::IsInSVGImage(const Node* node) {
  DCHECK(node);

  Page* page = node->GetDocument().GetPage();
  if (!page)
    return false;

  return page->GetChromeClient().IsSVGImageChromeClient();
}

void SVGImage::CheckLoaded() const {
  CHECK(page_);

  LocalFrame* frame = ToLocalFrame(page_->MainFrame());

  // Failures of this assertion might result in wrong origin tainting checks,
  // because CurrentFrameHasSingleSecurityOrigin() assumes all subresources of
  // the SVG are loaded and thus ready for origin checks.
  CHECK(frame->GetDocument()->LoadEventFinished());
}

bool SVGImage::CurrentFrameHasSingleSecurityOrigin() const {
  if (!page_)
    return true;

  LocalFrame* frame = ToLocalFrame(page_->MainFrame());

  CheckLoaded();

  SVGSVGElement* root_element =
      frame->GetDocument()->AccessSVGExtensions().rootElement();
  if (!root_element)
    return true;

  // Don't allow foreignObject elements or images that are not known to be
  // single-origin since these can leak cross-origin information.
  for (Node* node = root_element; node; node = FlatTreeTraversal::Next(*node)) {
    if (isSVGForeignObjectElement(*node))
      return false;
    if (isSVGImageElement(*node)) {
      if (!toSVGImageElement(*node).CurrentFrameHasSingleSecurityOrigin())
        return false;
    } else if (isSVGFEImageElement(*node)) {
      if (!toSVGFEImageElement(*node).CurrentFrameHasSingleSecurityOrigin())
        return false;
    }
  }

  // Because SVG image rendering disallows external resources and links, these
  // images effectively are restricted to a single security origin.
  return true;
}

static SVGSVGElement* SvgRootElement(Page* page) {
  if (!page)
    return nullptr;
  LocalFrame* frame = ToLocalFrame(page->MainFrame());
  return frame->GetDocument()->AccessSVGExtensions().rootElement();
}

IntSize SVGImage::ContainerSize() const {
  SVGSVGElement* root_element = SvgRootElement(page_.Get());
  if (!root_element)
    return IntSize();

  LayoutSVGRoot* layout_object =
      ToLayoutSVGRoot(root_element->GetLayoutObject());
  if (!layout_object)
    return IntSize();

  // If a container size is available it has precedence.
  IntSize container_size = layout_object->ContainerSize();
  if (!container_size.IsEmpty())
    return container_size;

  // Assure that a container size is always given for a non-identity zoom level.
  DCHECK_EQ(layout_object->Style()->EffectiveZoom(), 1);

  // No set container size; use concrete object size.
  return intrinsic_size_;
}

static float ResolveWidthForRatio(float height,
                                  const FloatSize& intrinsic_ratio) {
  return height * intrinsic_ratio.Width() / intrinsic_ratio.Height();
}

static float ResolveHeightForRatio(float width,
                                   const FloatSize& intrinsic_ratio) {
  return width * intrinsic_ratio.Height() / intrinsic_ratio.Width();
}

bool SVGImage::HasIntrinsicDimensions() const {
  return !ConcreteObjectSize(FloatSize()).IsEmpty();
}

FloatSize SVGImage::ConcreteObjectSize(
    const FloatSize& default_object_size) const {
  SVGSVGElement* svg = SvgRootElement(page_.Get());
  if (!svg)
    return FloatSize();

  LayoutSVGRoot* layout_object = ToLayoutSVGRoot(svg->GetLayoutObject());
  if (!layout_object)
    return FloatSize();

  LayoutReplaced::IntrinsicSizingInfo intrinsic_sizing_info;
  layout_object->ComputeIntrinsicSizingInfo(intrinsic_sizing_info);

  // https://www.w3.org/TR/css3-images/#default-sizing

  if (intrinsic_sizing_info.has_width && intrinsic_sizing_info.has_height)
    return intrinsic_sizing_info.size;

  if (svg->preserveAspectRatio()->CurrentValue()->Align() ==
      SVGPreserveAspectRatio::kSvgPreserveaspectratioNone) {
    // TODO(davve): The intrinsic aspect ratio is not used to resolve a missing
    // intrinsic width or height when preserveAspectRatio is none. It's unclear
    // whether this is correct. See crbug.com/584172.
    return default_object_size;
  }

  if (intrinsic_sizing_info.has_width) {
    if (intrinsic_sizing_info.aspect_ratio.IsEmpty())
      return FloatSize(intrinsic_sizing_info.size.Width(),
                       default_object_size.Height());

    return FloatSize(intrinsic_sizing_info.size.Width(),
                     ResolveHeightForRatio(intrinsic_sizing_info.size.Width(),
                                           intrinsic_sizing_info.aspect_ratio));
  }

  if (intrinsic_sizing_info.has_height) {
    if (intrinsic_sizing_info.aspect_ratio.IsEmpty())
      return FloatSize(default_object_size.Width(),
                       intrinsic_sizing_info.size.Height());

    return FloatSize(ResolveWidthForRatio(intrinsic_sizing_info.size.Height(),
                                          intrinsic_sizing_info.aspect_ratio),
                     intrinsic_sizing_info.size.Height());
  }

  if (!intrinsic_sizing_info.aspect_ratio.IsEmpty()) {
    // "A contain constraint is resolved by setting the concrete object size to
    //  the largest rectangle that has the object's intrinsic aspect ratio and
    //  additionally has neither width nor height larger than the constraint
    //  rectangle's width and height, respectively."
    float solution_width = ResolveWidthForRatio(
        default_object_size.Height(), intrinsic_sizing_info.aspect_ratio);
    if (solution_width <= default_object_size.Width())
      return FloatSize(solution_width, default_object_size.Height());

    float solution_height = ResolveHeightForRatio(
        default_object_size.Width(), intrinsic_sizing_info.aspect_ratio);
    return FloatSize(default_object_size.Width(), solution_height);
  }

  return default_object_size;
}

template <typename Func>
void SVGImage::ForContainer(const FloatSize& container_size, Func&& func) {
  if (!page_)
    return;

  // Temporarily disable the image observer to prevent changeInRect() calls due
  // re-laying out the image.
  ImageObserverDisabler image_observer_disabler(this);

  IntSize rounded_container_size = RoundedIntSize(container_size);

  if (SVGSVGElement* root_element = SvgRootElement(page_.Get())) {
    if (LayoutSVGRoot* layout_object =
            ToLayoutSVGRoot(root_element->GetLayoutObject()))
      layout_object->SetContainerSize(rounded_container_size);
  }

  func(FloatSize(rounded_container_size.Width() / container_size.Width(),
                 rounded_container_size.Height() / container_size.Height()));
}

void SVGImage::DrawForContainer(PaintCanvas* canvas,
                                const PaintFlags& flags,
                                const FloatSize& container_size,
                                float zoom,
                                const FloatRect& dst_rect,
                                const FloatRect& src_rect,
                                const KURL& url) {
  ForContainer(container_size, [&](const FloatSize& residual_scale) {
    FloatRect scaled_src = src_rect;
    scaled_src.Scale(1 / zoom);

    // Compensate for the container size rounding by adjusting the source rect.
    FloatSize adjusted_src_size = scaled_src.Size();
    adjusted_src_size.Scale(residual_scale.Width(), residual_scale.Height());
    scaled_src.SetSize(adjusted_src_size);

    DrawInternal(canvas, flags, dst_rect, scaled_src,
                 kDoNotRespectImageOrientation, kClampImageToSourceRect, url);
  });
}

PaintImage SVGImage::PaintImageForCurrentFrame() {
  PaintImageBuilder builder;
  InitPaintImageBuilder(builder);
  PopulatePaintRecordForCurrentFrameForContainer(builder, NullURL(), Size());
  return builder.TakePaintImage();
}

void SVGImage::DrawPatternForContainer(GraphicsContext& context,
                                       const FloatSize container_size,
                                       float zoom,
                                       const FloatRect& src_rect,
                                       const FloatSize& tile_scale,
                                       const FloatPoint& phase,
                                       SkBlendMode composite_op,
                                       const FloatRect& dst_rect,
                                       const FloatSize& repeat_spacing,
                                       const KURL& url) {
  // Tile adjusted for scaling/stretch.
  FloatRect tile(src_rect);
  tile.Scale(tile_scale.Width(), tile_scale.Height());

  // Expand the tile to account for repeat spacing.
  FloatRect spaced_tile(tile);
  spaced_tile.Expand(FloatSize(repeat_spacing));

  PaintRecordBuilder builder(spaced_tile, nullptr, &context);

  {
    DrawingRecorder recorder(builder.Context(), builder,
                             DisplayItem::Type::kSVGImage, spaced_tile);
    // When generating an expanded tile, make sure we don't draw into the
    // spacing area.
    if (tile != spaced_tile)
      builder.Context().Clip(tile);
    PaintFlags flags;
    DrawForContainer(builder.Context().Canvas(), flags, container_size, zoom,
                     tile, src_rect, url);
  }
  sk_sp<PaintRecord> record = builder.EndRecording();

  SkMatrix pattern_transform;
  pattern_transform.setTranslate(phase.X() + spaced_tile.X(),
                                 phase.Y() + spaced_tile.Y());

  PaintFlags flags;
  flags.setShader(PaintShader::MakePaintRecord(
      record, spaced_tile, SkShader::kRepeat_TileMode,
      SkShader::kRepeat_TileMode, &pattern_transform));
  // If the shader could not be instantiated (e.g. non-invertible matrix),
  // draw transparent.
  // Note: we can't simply bail, because of arbitrary blend mode.
  if (!flags.HasShader())
    flags.setColor(SK_ColorTRANSPARENT);

  flags.setBlendMode(composite_op);
  flags.setColorFilter(sk_ref_sp(context.GetColorFilter()));
  context.DrawRect(dst_rect, flags);
}

sk_sp<PaintRecord> SVGImage::PaintRecordForContainer(
    const KURL& url,
    const IntSize& container_size,
    const IntRect& draw_src_rect,
    const IntRect& draw_dst_rect,
    bool flip_y) {
  if (!page_)
    return nullptr;

  PaintRecorder recorder;
  PaintCanvas* canvas = recorder.beginRecording(draw_src_rect);
  if (flip_y) {
    canvas->translate(0, draw_dst_rect.Height());
    canvas->scale(1, -1);
  }
  DrawForContainer(canvas, PaintFlags(), FloatSize(container_size), 1,
                   draw_dst_rect, draw_src_rect, url);
  return recorder.finishRecordingAsPicture();
}

void SVGImage::PopulatePaintRecordForCurrentFrameForContainer(
    PaintImageBuilder& builder,
    const KURL& url,
    const IntSize& container_size) {
  if (!page_)
    return;

  const IntRect container_rect(IntPoint(), container_size);

  PaintRecorder recorder;
  PaintCanvas* canvas = recorder.beginRecording(container_rect);
  DrawForContainer(canvas, PaintFlags(), FloatSize(container_rect.Size()), 1,
                   container_rect, container_rect, url);
  builder.set_paint_record(recorder.finishRecordingAsPicture(), container_rect,
                           PaintImage::GetNextContentId());
}

static bool DrawNeedsLayer(const PaintFlags& flags) {
  if (SkColorGetA(flags.getColor()) < 255)
    return true;

  return flags.getBlendMode() != SkBlendMode::kSrcOver;
}

bool SVGImage::ApplyShaderInternal(PaintFlags& flags,
                                   const SkMatrix& local_matrix,
                                   const KURL& url) {
  const IntSize size(ContainerSize());
  if (size.IsEmpty())
    return false;

  IntRect bounds(IntPoint(), size);

  flags.setShader(PaintShader::MakePaintRecord(
      PaintRecordForCurrentFrame(bounds, url), bounds,
      SkShader::kRepeat_TileMode, SkShader::kRepeat_TileMode, &local_matrix));

  // Animation is normally refreshed in draw() impls, which we don't reach when
  // painting via shaders.
  StartAnimation();

  return true;
}

bool SVGImage::ApplyShader(PaintFlags& flags, const SkMatrix& local_matrix) {
  return ApplyShaderInternal(flags, local_matrix, NullURL());
}

bool SVGImage::ApplyShaderForContainer(const FloatSize& container_size,
                                       float zoom,
                                       const KURL& url,
                                       PaintFlags& flags,
                                       const SkMatrix& local_matrix) {
  bool result = false;
  ForContainer(container_size, [&](const FloatSize& residual_scale) {
    // Compensate for the container size rounding.
    auto adjusted_local_matrix = local_matrix;
    adjusted_local_matrix.preScale(zoom * residual_scale.Width(),
                                   zoom * residual_scale.Height());

    result = ApplyShaderInternal(flags, adjusted_local_matrix, url);
  });

  return result;
}

void SVGImage::Draw(
    PaintCanvas* canvas,
    const PaintFlags& flags,
    const FloatRect& dst_rect,
    const FloatRect& src_rect,
    RespectImageOrientationEnum should_respect_image_orientation,
    ImageClampingMode clamp_mode) {
  if (!page_)
    return;

  DrawInternal(canvas, flags, dst_rect, src_rect,
               should_respect_image_orientation, clamp_mode, NullURL());
}

sk_sp<PaintRecord> SVGImage::PaintRecordForCurrentFrame(const IntRect& bounds,
                                                        const KURL& url,
                                                        PaintCanvas* canvas) {
  DCHECK(page_);
  LocalFrameView* view = ToLocalFrame(page_->MainFrame())->View();
  view->Resize(ContainerSize());

  // Always call processUrlFragment, even if the url is empty, because
  // there may have been a previous url/fragment that needs to be reset.
  view->ProcessUrlFragment(url);

  // If the image was reset, we need to rewind the timeline back to 0. This
  // needs to be done before painting, or else we wouldn't get the correct
  // reset semantics (we'd paint the "last" frame rather than the one at
  // time=0.) The reason we do this here and not in resetAnimation() is to
  // avoid setting timers from the latter.
  FlushPendingTimelineRewind();

  PaintRecordBuilder builder(bounds, nullptr, nullptr, paint_controller_.get());

  view->UpdateAllLifecyclePhasesExceptPaint();
  view->Paint(builder.Context(), CullRect(bounds));
  DCHECK(!view->NeedsLayout());

  if (canvas) {
    builder.EndRecording(*canvas);
    return nullptr;
  }
  return builder.EndRecording();
}

void SVGImage::DrawInternal(PaintCanvas* canvas,
                            const PaintFlags& flags,
                            const FloatRect& dst_rect,
                            const FloatRect& src_rect,
                            RespectImageOrientationEnum,
                            ImageClampingMode,
                            const KURL& url) {
  {
    PaintCanvasAutoRestore ar(canvas, false);
    if (DrawNeedsLayer(flags)) {
      SkRect layer_rect = dst_rect;
      canvas->saveLayer(&layer_rect, &flags);
    }
    // We can only draw the entire frame, clipped to the rect we want. So
    // compute where the top left of the image would be if we were drawing
    // without clipping, and translate accordingly.
    FloatSize scale(dst_rect.Width() / src_rect.Width(),
                    dst_rect.Height() / src_rect.Height());
    FloatSize top_left_offset(src_rect.Location().X() * scale.Width(),
                              src_rect.Location().Y() * scale.Height());
    FloatPoint dest_offset = dst_rect.Location() - top_left_offset;
    AffineTransform transform =
        AffineTransform::Translation(dest_offset.X(), dest_offset.Y());
    transform.Scale(scale.Width(), scale.Height());

    canvas->save();
    canvas->clipRect(EnclosingIntRect(dst_rect));
    canvas->concat(AffineTransformToSkMatrix(transform));
    PaintRecordForCurrentFrame(EnclosingIntRect(src_rect), url, canvas);
    canvas->restore();
  }

  // Start any (SMIL) animations if needed. This will restart or continue
  // animations if preceded by calls to resetAnimation or stopAnimation
  // respectively.
  StartAnimation();
}

LayoutReplaced* SVGImage::EmbeddedReplacedContent() const {
  SVGSVGElement* root_element = SvgRootElement(page_.Get());
  if (!root_element)
    return nullptr;
  return ToLayoutSVGRoot(root_element->GetLayoutObject());
}

void SVGImage::ScheduleTimelineRewind() {
  has_pending_timeline_rewind_ = true;
}

void SVGImage::FlushPendingTimelineRewind() {
  if (!has_pending_timeline_rewind_)
    return;
  if (SVGSVGElement* root_element = SvgRootElement(page_.Get()))
    root_element->setCurrentTime(0);
  has_pending_timeline_rewind_ = false;
}

// FIXME: support CatchUpAnimation = CatchUp.
void SVGImage::StartAnimation(CatchUpAnimation) {
  SVGSVGElement* root_element = SvgRootElement(page_.Get());
  if (!root_element)
    return;
  chrome_client_->ResumeAnimation();
  if (root_element->animationsPaused())
    root_element->unpauseAnimations();
}

void SVGImage::StopAnimation() {
  SVGSVGElement* root_element = SvgRootElement(page_.Get());
  if (!root_element)
    return;
  chrome_client_->SuspendAnimation();
  root_element->pauseAnimations();
}

void SVGImage::ResetAnimation() {
  SVGSVGElement* root_element = SvgRootElement(page_.Get());
  if (!root_element)
    return;
  chrome_client_->SuspendAnimation();
  root_element->pauseAnimations();
  ScheduleTimelineRewind();
}

bool SVGImage::MaybeAnimated() {
  SVGSVGElement* root_element = SvgRootElement(page_.Get());
  if (!root_element)
    return false;
  return root_element->TimeContainer()->HasAnimations() ||
         ToLocalFrame(page_->MainFrame())
             ->GetDocument()
             ->Timeline()
             .HasPendingUpdates();
}

void SVGImage::ServiceAnimations(double monotonic_animation_start_time) {
  if (!GetImageObserver())
    return;

  // If none of our observers (sic!) are visible, or for some other reason
  // does not want us to keep running animations, stop them until further
  // notice (next paint.)
  if (GetImageObserver()->ShouldPauseAnimation(this)) {
    StopAnimation();
    return;
  }

  // serviceScriptedAnimations runs requestAnimationFrame callbacks, but SVG
  // images can't have any so we assert there's no script.
  ScriptForbiddenScope forbid_script;

  // The calls below may trigger GCs, so set up the required persistent
  // reference on the ImageResourceContent which owns this SVGImage. By
  // transitivity, that will keep the associated SVGImageChromeClient object
  // alive.
  Persistent<ImageObserver> protect(GetImageObserver());
  page_->Animator().ServiceScriptedAnimations(monotonic_animation_start_time);
  // Do *not* update the paint phase. It's critical to paint only when
  // actually generating painted output, not only for performance reasons,
  // but to preserve correct coherence of the cache of the output with
  // the needsRepaint bits of the PaintLayers in the image.
  LocalFrameView* frame_view = ToLocalFrame(page_->MainFrame())->View();
  frame_view->UpdateAllLifecyclePhasesExceptPaint();

  if (RuntimeEnabledFeatures::SlimmingPaintV2Enabled()) {
    // For SPv2 we run UpdateAnimations after the paint phase, but per above
    // comment we don't want to run lifecycle through to paint for SVG images.
    // Since we know SVG images never have composited animations we can update
    // animations directly without worrying about including
    // PaintArtifactCompositor analysis of whether animations should be
    // composited.
    Optional<CompositorElementIdSet> composited_element_ids;
    DocumentAnimations::UpdateAnimations(
        frame_view->GetLayoutView()->GetDocument(),
        DocumentLifecycle::kLayoutClean, composited_element_ids);

    // Notify observers for image change. In SPv1 this is done through window
    // rect invalidation during paint invalidation of the SVGImage's frame view.
    auto* layer = frame_view->GetLayoutView()->Layer();
    if (layer->NeedsRepaint()) {
      if (auto* observer = GetImageObserver())
        observer->ChangedInRect(this, Rect());
      layer->ClearNeedsRepaintRecursively();
    }
  }
}

void SVGImage::AdvanceAnimationForTesting() {
  if (SVGSVGElement* root_element = SvgRootElement(page_.Get())) {
    root_element->TimeContainer()->AdvanceFrameForTesting();

    // The following triggers animation updates which can issue a new draw
    // but will not permanently change the animation timeline.
    // TODO(pdr): Actually advance the document timeline so CSS animations
    // can be properly tested.
    page_->Animator().ServiceScriptedAnimations(root_element->getCurrentTime());
    GetImageObserver()->AnimationAdvanced(this);
  }
}

SVGImageChromeClient& SVGImage::ChromeClientForTesting() {
  return *chrome_client_;
}

void SVGImage::UpdateUseCounters(const Document& document) const {
  if (SVGSVGElement* root_element = SvgRootElement(page_.Get())) {
    if (root_element->TimeContainer()->HasAnimations()) {
      UseCounter::Count(document,
                        WebFeature::kSVGSMILAnimationInImageRegardlessOfCache);
    }
  }
}

void SVGImage::LoadCompleted() {
  switch (load_state_) {
    case kInDataChanged:
      load_state_ = kLoadCompleted;
      break;

    case kWaitingForAsyncLoadCompletion:
      load_state_ = kLoadCompleted;

      // Because LoadCompleted() is called synchronously from
      // Document::ImplicitClose(), we defer AsyncLoadCompleted() to avoid
      // potential bugs and timing dependencies around ImplicitClose() and
      // to make LoadEventFinished() true when AsyncLoadCompleted() is called.
      TaskRunnerHelper::Get(TaskType::kUnspecedLoading,
                            ToLocalFrame(page_->MainFrame()))
          ->PostTask(BLINK_FROM_HERE,
                     WTF::Bind(&SVGImage::NotifyAsyncLoadCompleted,
                               RefPtr<SVGImage>(this)));
      break;

    case kDataChangedNotStarted:
    case kLoadCompleted:
      CHECK(false);
      break;
  }
}

void SVGImage::NotifyAsyncLoadCompleted() {
  if (GetImageObserver())
    GetImageObserver()->AsyncLoadCompleted(this);
}

Image::SizeAvailability SVGImage::DataChanged(bool all_data_received) {
  TRACE_EVENT0("blink", "SVGImage::dataChanged");

  // Don't do anything if is an empty image.
  if (!Data()->size())
    return kSizeAvailable;

  if (!all_data_received)
    return page_ ? kSizeAvailable : kSizeUnavailable;

  CHECK(!page_);

  // SVGImage will fire events (and the default C++ handlers run) but doesn't
  // actually allow script to run so it's fine to call into it. We allow this
  // since it means an SVG data url can synchronously load like other image
  // types.
  EventDispatchForbiddenScope::AllowUserAgentEvents allow_user_agent_events;

  CHECK_EQ(load_state_, kDataChangedNotStarted);
  load_state_ = kInDataChanged;

  Page::PageClients page_clients;
  FillWithEmptyClients(page_clients);
  chrome_client_ = SVGImageChromeClient::Create(this);
  page_clients.chrome_client = chrome_client_.Get();

  // FIXME: If this SVG ends up loading itself, we might leak the world.
  // The Cache code does not know about ImageResources holding Frames and
  // won't know to break the cycle.
  // This will become an issue when SVGImage will be able to load other
  // SVGImage objects, but we're safe now, because SVGImage can only be
  // loaded by a top-level document.
  Page* page;
  {
    TRACE_EVENT0("blink", "SVGImage::dataChanged::createPage");
    page = Page::Create(page_clients);
    page->GetSettings().SetScriptEnabled(false);
    page->GetSettings().SetPluginsEnabled(false);
    page->GetSettings().SetAcceleratedCompositingEnabled(false);

    // Because this page is detached, it can't get default font settings
    // from the embedder. Copy over font settings so we have sensible
    // defaults. These settings are fixed and will not update if changed.
    if (!Page::OrdinaryPages().IsEmpty()) {
      Settings& default_settings =
          (*Page::OrdinaryPages().begin())->GetSettings();
      page->GetSettings().GetGenericFontFamilySettings() =
          default_settings.GetGenericFontFamilySettings();
      page->GetSettings().SetMinimumFontSize(
          default_settings.GetMinimumFontSize());
      page->GetSettings().SetMinimumLogicalFontSize(
          default_settings.GetMinimumLogicalFontSize());
      page->GetSettings().SetDefaultFontSize(
          default_settings.GetDefaultFontSize());
      page->GetSettings().SetDefaultFixedFontSize(
          default_settings.GetDefaultFixedFontSize());
    }
    }

    LocalFrame* frame = nullptr;
    {
      TRACE_EVENT0("blink", "SVGImage::dataChanged::createFrame");
      DCHECK(!frame_client_);
      frame_client_ = new SVGImageLocalFrameClient(this);
      frame = LocalFrame::Create(frame_client_, *page, 0);
      frame->SetView(LocalFrameView::Create(*frame));
      frame->Init();
    }

    FrameLoader& loader = frame->Loader();
    loader.ForceSandboxFlags(kSandboxAll);

    frame->View()->SetScrollbarsSuppressed(true);
    // SVG Images will always synthesize a viewBox, if it's not available, and
    // thus never see scrollbars.
    frame->View()->SetCanHaveScrollbars(false);
    // SVG Images are transparent.
    frame->View()->SetBaseBackgroundColor(Color::kTransparent);

    page_ = page;

    TRACE_EVENT0("blink", "SVGImage::dataChanged::load");
    loader.Load(
        FrameLoadRequest(0, ResourceRequest(BlankURL()),
                         SubstituteData(Data(), AtomicString("image/svg+xml"),
                                        AtomicString("UTF-8"), NullURL(),
                                        kForceSynchronousLoad)));

    // Set the concrete object size before a container size is available.
    intrinsic_size_ = RoundedIntSize(ConcreteObjectSize(FloatSize(
        LayoutReplaced::kDefaultWidth, LayoutReplaced::kDefaultHeight)));

    DCHECK(page_);
    switch (load_state_) {
      case kInDataChanged:
        load_state_ = kWaitingForAsyncLoadCompletion;
        return kSizeAvailableAndLoadingAsynchronously;

      case kLoadCompleted:
        return kSizeAvailable;

      case kDataChangedNotStarted:
      case kWaitingForAsyncLoadCompletion:
        CHECK(false);
        break;
  }

  NOTREACHED();
  return kSizeAvailable;
}

String SVGImage::FilenameExtension() const {
  return "svg";
}

}  // namespace blink
