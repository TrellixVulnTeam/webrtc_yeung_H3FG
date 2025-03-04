/*
 * Copyright (C) 2011, 2012 Apple Inc.  All rights reserved.
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
 */

#include "core/html/track/TextTrackList.h"

#include "bindings/core/v8/ExceptionState.h"
#include "core/dom/events/MediaElementEventQueue.h"
#include "core/html/HTMLMediaElement.h"
#include "core/html/track/InbandTextTrack.h"
#include "core/html/track/LoadableTextTrack.h"
#include "core/html/track/TextTrack.h"
#include "core/html/track/TrackEvent.h"

namespace blink {

TextTrackList::TextTrackList(HTMLMediaElement* owner)
    : owner_(owner), async_event_queue_(MediaElementEventQueue::Create(this)) {}

TextTrackList::~TextTrackList() {}

unsigned TextTrackList::length() const {
  return add_track_tracks_.size() + element_tracks_.size() +
         inband_tracks_.size();
}

int TextTrackList::GetTrackIndex(TextTrack* text_track) {
  if (text_track->TrackType() == TextTrack::kTrackElement)
    return ToLoadableTextTrack(text_track)->TrackElementIndex();

  if (text_track->TrackType() == TextTrack::kAddTrack)
    return element_tracks_.size() + add_track_tracks_.Find(text_track);

  if (text_track->TrackType() == TextTrack::kInBand)
    return element_tracks_.size() + add_track_tracks_.size() +
           inband_tracks_.Find(text_track);

  NOTREACHED();

  return -1;
}

int TextTrackList::GetTrackIndexRelativeToRenderedTracks(
    TextTrack* text_track) {
  // Calculate the "Let n be the number of text tracks whose text track mode is
  // showing and that are in the media element's list of text tracks before
  // track."
  int track_index = 0;

  for (const auto& track : element_tracks_) {
    if (!track->IsRendered())
      continue;

    if (track == text_track)
      return track_index;
    ++track_index;
  }

  for (const auto& track : add_track_tracks_) {
    if (!track->IsRendered())
      continue;

    if (track == text_track)
      return track_index;
    ++track_index;
  }

  for (const auto& track : inband_tracks_) {
    if (!track->IsRendered())
      continue;

    if (track == text_track)
      return track_index;
    ++track_index;
  }

  NOTREACHED();

  return -1;
}

TextTrack* TextTrackList::AnonymousIndexedGetter(unsigned index) {
  // 4.8.10.12.1 Text track model
  // The text tracks are sorted as follows:
  // 1. The text tracks corresponding to track element children of the media
  // element, in tree order.
  // 2. Any text tracks added using the addTextTrack() method, in the order they
  // were added, oldest first.
  // 3. Any media-resource-specific text tracks (text tracks corresponding to
  // data in the media resource), in the order defined by the media resource's
  // format specification.

  if (index < element_tracks_.size())
    return element_tracks_[index];

  index -= element_tracks_.size();
  if (index < add_track_tracks_.size())
    return add_track_tracks_[index];

  index -= add_track_tracks_.size();
  if (index < inband_tracks_.size())
    return inband_tracks_[index];

  return 0;
}

TextTrack* TextTrackList::getTrackById(const AtomicString& id) {
  // 4.8.10.12.5 Text track API
  // The getTrackById(id) method must return the first TextTrack in the
  // TextTrackList object whose id IDL attribute would return a value equal
  // to the value of the id argument.
  for (unsigned i = 0; i < length(); ++i) {
    TextTrack* track = AnonymousIndexedGetter(i);
    if (String(track->id()) == id)
      return track;
  }

  // When no tracks match the given argument, the method must return null.
  return 0;
}

void TextTrackList::InvalidateTrackIndexesAfterTrack(TextTrack* track) {
  HeapVector<TraceWrapperMember<TextTrack>>* tracks = nullptr;

  if (track->TrackType() == TextTrack::kTrackElement) {
    tracks = &element_tracks_;
    for (const auto& add_track : add_track_tracks_)
      add_track->InvalidateTrackIndex();
    for (const auto& inband_track : inband_tracks_)
      inband_track->InvalidateTrackIndex();
  } else if (track->TrackType() == TextTrack::kAddTrack) {
    tracks = &add_track_tracks_;
    for (const auto& inband_track : inband_tracks_)
      inband_track->InvalidateTrackIndex();
  } else if (track->TrackType() == TextTrack::kInBand) {
    tracks = &inband_tracks_;
  } else {
    NOTREACHED();
  }

  size_t index = tracks->Find(track);
  if (index == kNotFound)
    return;

  for (size_t i = index; i < tracks->size(); ++i)
    tracks->at(i)->InvalidateTrackIndex();
}

void TextTrackList::Append(TextTrack* track) {
  if (track->TrackType() == TextTrack::kAddTrack) {
    add_track_tracks_.push_back(track);
  } else if (track->TrackType() == TextTrack::kTrackElement) {
    // Insert tracks added for <track> element in tree order.
    size_t index = ToLoadableTextTrack(track)->TrackElementIndex();
    element_tracks_.insert(index, track);
  } else if (track->TrackType() == TextTrack::kInBand) {
    inband_tracks_.push_back(track);
  } else {
    NOTREACHED();
  }

  InvalidateTrackIndexesAfterTrack(track);

  DCHECK(!track->TrackList());
  track->SetTrackList(this);

  ScheduleAddTrackEvent(track);
}

void TextTrackList::Remove(TextTrack* track) {
  HeapVector<TraceWrapperMember<TextTrack>>* tracks = nullptr;

  if (track->TrackType() == TextTrack::kTrackElement) {
    tracks = &element_tracks_;
  } else if (track->TrackType() == TextTrack::kAddTrack) {
    tracks = &add_track_tracks_;
  } else if (track->TrackType() == TextTrack::kInBand) {
    tracks = &inband_tracks_;
  } else {
    NOTREACHED();
  }

  size_t index = tracks->Find(track);
  if (index == kNotFound)
    return;

  InvalidateTrackIndexesAfterTrack(track);

  DCHECK_EQ(track->TrackList(), this);
  track->SetTrackList(0);

  tracks->erase(index);

  ScheduleRemoveTrackEvent(track);
}

void TextTrackList::RemoveAllInbandTracks() {
  for (const auto& track : inband_tracks_) {
    track->SetTrackList(0);
  }
  inband_tracks_.clear();
}

bool TextTrackList::Contains(TextTrack* track) const {
  const HeapVector<TraceWrapperMember<TextTrack>>* tracks = nullptr;

  if (track->TrackType() == TextTrack::kTrackElement)
    tracks = &element_tracks_;
  else if (track->TrackType() == TextTrack::kAddTrack)
    tracks = &add_track_tracks_;
  else if (track->TrackType() == TextTrack::kInBand)
    tracks = &inband_tracks_;
  else
    NOTREACHED();

  return tracks->Find(track) != kNotFound;
}

const AtomicString& TextTrackList::InterfaceName() const {
  return EventTargetNames::TextTrackList;
}

ExecutionContext* TextTrackList::GetExecutionContext() const {
  return owner_ ? owner_->GetExecutionContext() : 0;
}

void TextTrackList::ScheduleTrackEvent(const AtomicString& event_name,
                                       TextTrack* track) {
  async_event_queue_->EnqueueEvent(BLINK_FROM_HERE,
                                   TrackEvent::Create(event_name, track));
}

void TextTrackList::ScheduleAddTrackEvent(TextTrack* track) {
  // 4.8.10.12.3 Sourcing out-of-band text tracks
  // 4.8.10.12.4 Text track API
  // ... then queue a task to fire an event with the name addtrack, that does
  // not bubble and is not cancelable, and that uses the TrackEvent interface,
  // with the track attribute initialized to the text track's TextTrack object,
  // at the media element's textTracks attribute's TextTrackList object.
  ScheduleTrackEvent(EventTypeNames::addtrack, track);
}

void TextTrackList::ScheduleChangeEvent() {
  // 4.8.10.12.1 Text track model
  // Whenever a text track that is in a media element's list of text tracks
  // has its text track mode change value, the user agent must run the
  // following steps for the media element:
  // ...
  // Fire a simple event named change at the media element's textTracks
  // attribute's TextTrackList object.

  async_event_queue_->EnqueueEvent(BLINK_FROM_HERE,
                                   Event::Create(EventTypeNames::change));
}

void TextTrackList::ScheduleRemoveTrackEvent(TextTrack* track) {
  // 4.8.10.12.3 Sourcing out-of-band text tracks
  // When a track element's parent element changes and the old parent was a
  // media element, then the user agent must remove the track element's
  // corresponding text track from the media element's list of text tracks,
  // and then queue a task to fire a trusted event with the name removetrack,
  // that does not bubble and is not cancelable, and that uses the TrackEvent
  // interface, with the track attribute initialized to the text track's
  // TextTrack object, at the media element's textTracks attribute's
  // TextTrackList object.
  ScheduleTrackEvent(EventTypeNames::removetrack, track);
}

bool TextTrackList::HasShowingTracks() {
  for (unsigned i = 0; i < length(); ++i) {
    if (AnonymousIndexedGetter(i)->mode() == TextTrack::ShowingKeyword())
      return true;
  }
  return false;
}

HTMLMediaElement* TextTrackList::Owner() const {
  return owner_;
}

DEFINE_TRACE(TextTrackList) {
  visitor->Trace(owner_);
  visitor->Trace(async_event_queue_);
  visitor->Trace(add_track_tracks_);
  visitor->Trace(element_tracks_);
  visitor->Trace(inband_tracks_);
  EventTargetWithInlineData::Trace(visitor);
}

DEFINE_TRACE_WRAPPERS(TextTrackList) {
  for (auto track : add_track_tracks_)
    visitor->TraceWrappers(track);
  for (auto track : element_tracks_)
    visitor->TraceWrappers(track);
  for (auto track : inband_tracks_)
    visitor->TraceWrappers(track);
  EventTargetWithInlineData::TraceWrappers(visitor);
}

}  // namespace blink
