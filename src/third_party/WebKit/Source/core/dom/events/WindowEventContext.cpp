/*
 * Copyright (C) 2010 Google Inc. All Rights Reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "core/dom/events/WindowEventContext.h"

#include "core/dom/Document.h"
#include "core/dom/Node.h"
#include "core/dom/events/Event.h"
#include "core/events/NodeEventContext.h"
#include "core/frame/LocalDOMWindow.h"

namespace blink {

WindowEventContext::WindowEventContext(
    Event& event,
    const NodeEventContext& top_node_event_context) {
  // We don't dispatch load events to the window. This quirk was originally
  // added because Mozilla doesn't propagate load events to the window object.
  if (event.type() == EventTypeNames::load)
    return;
  if (!top_node_event_context.GetNode()->IsDocumentNode())
    return;
  window_ = ToDocument(top_node_event_context.GetNode())->domWindow();
  target_ = top_node_event_context.Target();
}

bool WindowEventContext::HandleLocalEvents(Event& event) {
  if (!window_)
    return false;

  event.SetTarget(Target());
  event.SetCurrentTarget(Window());
  window_->FireEventListeners(&event);
  return true;
}

DEFINE_TRACE(WindowEventContext) {
  visitor->Trace(window_);
  visitor->Trace(target_);
}

}  // namespace blink
