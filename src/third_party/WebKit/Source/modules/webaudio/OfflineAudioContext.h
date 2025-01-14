/*
 * Copyright (C) 2012, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#ifndef OfflineAudioContext_h
#define OfflineAudioContext_h

#include "modules/ModulesExport.h"
#include "modules/webaudio/BaseAudioContext.h"
#include "platform/wtf/HashMap.h"

namespace blink {

class ExceptionState;
class OfflineAudioContextOptions;
class OfflineAudioDestinationHandler;

class MODULES_EXPORT OfflineAudioContext final : public BaseAudioContext {
  DEFINE_WRAPPERTYPEINFO();

 public:
  static OfflineAudioContext* Create(ExecutionContext*,
                                     unsigned number_of_channels,
                                     unsigned number_of_frames,
                                     float sample_rate,
                                     ExceptionState&);

  static OfflineAudioContext* Create(ExecutionContext*,
                                     const OfflineAudioContextOptions&,
                                     ExceptionState&);

  ~OfflineAudioContext() override;

  DECLARE_VIRTUAL_TRACE();

  size_t length() const { return total_render_frames_; }

  ScriptPromise startOfflineRendering(ScriptState*);

  ScriptPromise suspendContext(ScriptState*, double);
  ScriptPromise resumeContext(ScriptState*) final;

  // This is to implement the pure virtual method from BaseAudioContext.
  // CANNOT be called from an OfflineAudioContext.
  ScriptPromise suspendContext(ScriptState*) final;

  void RejectPendingResolvers() override;

  bool HasRealtimeConstraint() final { return false; }

  DEFINE_ATTRIBUTE_EVENT_LISTENER(complete);

  // Fire completion event when the rendering is finished.
  void FireCompletionEvent();

  // This is same with the online version in BaseAudioContext class except
  // for returning a boolean value after checking the scheduled suspends.
  bool HandlePreOfflineRenderTasks();

  void HandlePostOfflineRenderTasks();

  // Resolve a suspend scheduled at the specified frame. With this specified
  // frame as a unique key, the associated promise resolver can be retrieved
  // from the map (m_scheduledSuspends) and resolved.
  void ResolveSuspendOnMainThread(size_t);

  // The HashMap with 'zero' key is needed because |currentSampleFrame| can be
  // zero.
  using SuspendMap = HeapHashMap<size_t,
                                 Member<ScriptPromiseResolver>,
                                 DefaultHash<size_t>::Hash,
                                 WTF::UnsignedWithZeroKeyHashTraits<size_t>>;

  using OfflineGraphAutoLocker = DeferredTaskHandler::OfflineGraphAutoLocker;

 private:
  OfflineAudioContext(Document*,
                      unsigned number_of_channels,
                      size_t number_of_frames,
                      float sample_rate,
                      ExceptionState&);

  // Fetch directly the destination handler.
  OfflineAudioDestinationHandler& DestinationHandler();

  // Check if the rendering needs to be suspended.
  bool ShouldSuspend();

  // This map is to store the timing of scheduled suspends (frame) and the
  // associated promise resolver. This storage can only be modified by the
  // main thread and accessed by the audio thread with the graph lock.
  //
  // The map consists of key-value pairs of:
  // { size_t quantizedFrame: ScriptPromiseResolver resolver }
  //
  // Note that |quantizedFrame| is a unique key, since you can have only one
  // suspend scheduled for a certain frame. Accessing to this must be
  // protected by the offline context lock.
  SuspendMap scheduled_suspends_;

  Member<ScriptPromiseResolver> complete_resolver_;

  // This flag is necessary to indicate the rendering has actually started.
  // Note that initial state of context is 'Suspended', which is the same
  // state when the context is suspended.
  bool is_rendering_started_;

  // Total render sample length.
  size_t total_render_frames_;
};

}  // namespace blink

#endif  // OfflineAudioContext_h
