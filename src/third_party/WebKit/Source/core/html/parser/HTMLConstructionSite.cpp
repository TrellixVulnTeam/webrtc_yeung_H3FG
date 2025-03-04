/*
 * Copyright (C) 2010 Google, Inc. All Rights Reserved.
 * Copyright (C) 2011 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY GOOGLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL GOOGLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/html/parser/HTMLConstructionSite.h"

#include <limits>
#include "core/HTMLElementFactory.h"
#include "core/HTMLNames.h"
#include "core/dom/Comment.h"
#include "core/dom/DocumentFragment.h"
#include "core/dom/DocumentType.h"
#include "core/dom/Element.h"
#include "core/dom/ElementTraversal.h"
#include "core/dom/IgnoreDestructiveWriteCountIncrementer.h"
#include "core/dom/TemplateContentDocumentFragment.h"
#include "core/dom/Text.h"
#include "core/dom/ThrowOnDynamicMarkupInsertionCountIncrementer.h"
#include "core/frame/LocalDOMWindow.h"
#include "core/frame/LocalFrame.h"
#include "core/frame/LocalFrameClient.h"
#include "core/html/FormAssociated.h"
#include "core/html/HTMLFormElement.h"
#include "core/html/HTMLHtmlElement.h"
#include "core/html/HTMLPlugInElement.h"
#include "core/html/HTMLScriptElement.h"
#include "core/html/HTMLTemplateElement.h"
#include "core/html/custom/CEReactionsScope.h"
#include "core/html/custom/CustomElementDefinition.h"
#include "core/html/custom/CustomElementDescriptor.h"
#include "core/html/custom/CustomElementRegistry.h"
#include "core/html/parser/AtomicHTMLToken.h"
#include "core/html/parser/HTMLParserIdioms.h"
#include "core/html/parser/HTMLParserReentryPermit.h"
#include "core/html/parser/HTMLStackItem.h"
#include "core/html/parser/HTMLToken.h"
#include "core/loader/FrameLoader.h"
#include "core/svg/SVGScriptElement.h"
#include "platform/bindings/Microtask.h"
#include "platform/bindings/V8PerIsolateData.h"
#include "platform/text/TextBreakIterator.h"

namespace blink {

using namespace HTMLNames;

static const unsigned kMaximumHTMLParserDOMTreeDepth = 512;

static inline void SetAttributes(Element* element,
                                 AtomicHTMLToken* token,
                                 ParserContentPolicy parser_content_policy) {
  if (!ScriptingContentIsAllowed(parser_content_policy))
    element->StripScriptingAttributes(token->Attributes());
  element->ParserSetAttributes(token->Attributes());
}

static bool HasImpliedEndTag(const HTMLStackItem* item) {
  return item->HasTagName(ddTag) || item->HasTagName(dtTag) ||
         item->HasTagName(liTag) || item->HasTagName(optionTag) ||
         item->HasTagName(optgroupTag) || item->HasTagName(pTag) ||
         item->HasTagName(rbTag) || item->HasTagName(rpTag) ||
         item->HasTagName(rtTag) || item->HasTagName(rtcTag);
}

static bool ShouldUseLengthLimit(const ContainerNode& node) {
  return !isHTMLScriptElement(node) && !isHTMLStyleElement(node) &&
         !isSVGScriptElement(node);
}

static unsigned TextLengthLimitForContainer(const ContainerNode& node) {
  return ShouldUseLengthLimit(node) ? Text::kDefaultLengthLimit
                                    : std::numeric_limits<unsigned>::max();
}

static inline bool IsAllWhitespace(const String& string) {
  return string.IsAllSpecialCharacters<IsHTMLSpace<UChar>>();
}

static inline void Insert(HTMLConstructionSiteTask& task) {
  if (isHTMLTemplateElement(*task.parent))
    task.parent = toHTMLTemplateElement(task.parent.Get())->content();

  // https://html.spec.whatwg.org/#insert-a-foreign-element
  // 3.1, (3) Push (pop) an element queue
  CEReactionsScope reactions;
  if (task.next_child)
    task.parent->ParserInsertBefore(task.child.Get(), *task.next_child);
  else
    task.parent->ParserAppendChild(task.child.Get());
}

static inline void ExecuteInsertTask(HTMLConstructionSiteTask& task) {
  DCHECK_EQ(task.operation, HTMLConstructionSiteTask::kInsert);

  Insert(task);

  if (task.child->IsElementNode()) {
    Element& child = ToElement(*task.child);
    child.BeginParsingChildren();
    if (task.self_closing)
      child.FinishParsingChildren();
  }
}

static inline void ExecuteInsertTextTask(HTMLConstructionSiteTask& task) {
  DCHECK_EQ(task.operation, HTMLConstructionSiteTask::kInsertText);
  DCHECK(task.child->IsTextNode());

  // Merge text nodes into previous ones if possible:
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/tree-construction.html#insert-a-character
  Text* new_text = ToText(task.child.Get());
  Node* previous_child = task.next_child ? task.next_child->previousSibling()
                                         : task.parent->lastChild();
  if (previous_child && previous_child->IsTextNode()) {
    Text* previous_text = ToText(previous_child);
    unsigned length_limit = TextLengthLimitForContainer(*task.parent);
    if (previous_text->length() + new_text->length() < length_limit) {
      previous_text->ParserAppendData(new_text->data());
      return;
    }
  }

  Insert(task);
}

static inline void ExecuteReparentTask(HTMLConstructionSiteTask& task) {
  DCHECK_EQ(task.operation, HTMLConstructionSiteTask::kReparent);

  task.parent->ParserAppendChild(task.child);
}

static inline void ExecuteInsertAlreadyParsedChildTask(
    HTMLConstructionSiteTask& task) {
  DCHECK_EQ(task.operation,
            HTMLConstructionSiteTask::kInsertAlreadyParsedChild);

  Insert(task);
}

static inline void ExecuteTakeAllChildrenTask(HTMLConstructionSiteTask& task) {
  DCHECK_EQ(task.operation, HTMLConstructionSiteTask::kTakeAllChildren);

  task.parent->ParserTakeAllChildrenFrom(*task.OldParent());
}

void HTMLConstructionSite::ExecuteTask(HTMLConstructionSiteTask& task) {
  DCHECK(task_queue_.IsEmpty());
  if (task.operation == HTMLConstructionSiteTask::kInsert)
    return ExecuteInsertTask(task);

  if (task.operation == HTMLConstructionSiteTask::kInsertText)
    return ExecuteInsertTextTask(task);

  // All the cases below this point are only used by the adoption agency.

  if (task.operation == HTMLConstructionSiteTask::kInsertAlreadyParsedChild)
    return ExecuteInsertAlreadyParsedChildTask(task);

  if (task.operation == HTMLConstructionSiteTask::kReparent)
    return ExecuteReparentTask(task);

  if (task.operation == HTMLConstructionSiteTask::kTakeAllChildren)
    return ExecuteTakeAllChildrenTask(task);

  NOTREACHED();
}

// This is only needed for TextDocuments where we might have text nodes
// approaching the default length limit (~64k) and we don't want to break a text
// node in the middle of a combining character.
static unsigned FindBreakIndexBetween(const StringBuilder& string,
                                      unsigned current_position,
                                      unsigned proposed_break_index) {
  DCHECK_LT(current_position, proposed_break_index);
  DCHECK_LE(proposed_break_index, string.length());
  // The end of the string is always a valid break.
  if (proposed_break_index == string.length())
    return proposed_break_index;

  // Latin-1 does not have breakable boundaries. If we ever moved to a different
  // 8-bit encoding this could be wrong.
  if (string.Is8Bit())
    return proposed_break_index;

  const UChar* break_search_characters =
      string.Characters16() + current_position;
  // We need at least two characters look-ahead to account for UTF-16
  // surrogates, but can't search off the end of the buffer!
  unsigned break_search_length =
      std::min(proposed_break_index - current_position + 2,
               string.length() - current_position);
  NonSharedCharacterBreakIterator it(break_search_characters,
                                     break_search_length);

  if (it.IsBreak(proposed_break_index - current_position))
    return proposed_break_index;

  int adjusted_break_index_in_substring =
      it.Preceding(proposed_break_index - current_position);
  if (adjusted_break_index_in_substring > 0)
    return current_position + adjusted_break_index_in_substring;
  // We failed to find a breakable point, let the caller figure out what to do.
  return 0;
}

static String AtomizeIfAllWhitespace(const String& string,
                                     WhitespaceMode whitespace_mode) {
  // Strings composed entirely of whitespace are likely to be repeated. Turn
  // them into AtomicString so we share a single string for each.
  if (whitespace_mode == kAllWhitespace ||
      (whitespace_mode == kWhitespaceUnknown && IsAllWhitespace(string)))
    return AtomicString(string).GetString();
  return string;
}

void HTMLConstructionSite::FlushPendingText(FlushMode mode) {
  if (pending_text_.IsEmpty())
    return;

  if (mode == kFlushIfAtTextLimit &&
      !ShouldUseLengthLimit(*pending_text_.parent))
    return;

  PendingText pending_text;
  // Hold onto the current pending text on the stack so that queueTask doesn't
  // recurse infinitely.
  pending_text_.Swap(pending_text);
  DCHECK(pending_text_.IsEmpty());

  // Splitting text nodes into smaller chunks contradicts HTML5 spec, but is
  // necessary for performance, see:
  // https://bugs.webkit.org/show_bug.cgi?id=55898
  unsigned length_limit = TextLengthLimitForContainer(*pending_text.parent);

  unsigned current_position = 0;
  const StringBuilder& string = pending_text.string_builder;
  while (current_position < string.length()) {
    unsigned proposed_break_index =
        std::min(current_position + length_limit, string.length());
    unsigned break_index =
        FindBreakIndexBetween(string, current_position, proposed_break_index);
    DCHECK_LE(break_index, string.length());
    String substring =
        string.Substring(current_position, break_index - current_position);
    substring = AtomizeIfAllWhitespace(substring, pending_text.whitespace_mode);

    HTMLConstructionSiteTask task(HTMLConstructionSiteTask::kInsertText);
    task.parent = pending_text.parent;
    task.next_child = pending_text.next_child;
    task.child = Text::Create(task.parent->GetDocument(), substring);
    QueueTask(task);

    DCHECK_GT(break_index, current_position);
    DCHECK_EQ(break_index - current_position, substring.length());
    DCHECK_EQ(ToText(task.child.Get())->length(), substring.length());
    current_position = break_index;
  }
}

void HTMLConstructionSite::QueueTask(const HTMLConstructionSiteTask& task) {
  FlushPendingText(kFlushAlways);
  DCHECK(pending_text_.IsEmpty());
  task_queue_.push_back(task);
}

void HTMLConstructionSite::AttachLater(ContainerNode* parent,
                                       Node* child,
                                       bool self_closing) {
  DCHECK(ScriptingContentIsAllowed(parser_content_policy_) ||
         !child->IsElementNode() || !ToElement(child)->IsScriptElement());
  DCHECK(PluginContentIsAllowed(parser_content_policy_) ||
         !IsHTMLPlugInElement(child));

  HTMLConstructionSiteTask task(HTMLConstructionSiteTask::kInsert);
  task.parent = parent;
  task.child = child;
  task.self_closing = self_closing;

  if (ShouldFosterParent()) {
    FosterParent(task.child);
    return;
  }

  // Add as a sibling of the parent if we have reached the maximum depth
  // allowed.
  if (open_elements_.StackDepth() > kMaximumHTMLParserDOMTreeDepth &&
      task.parent->parentNode())
    task.parent = task.parent->parentNode();

  DCHECK(task.parent);
  QueueTask(task);
}

void HTMLConstructionSite::ExecuteQueuedTasks() {
  // This has no affect on pendingText, and we may have pendingText remaining
  // after executing all other queued tasks.
  const size_t size = task_queue_.size();
  if (!size)
    return;

  // Copy the task queue into a local variable in case executeTask re-enters the
  // parser.
  TaskQueue queue;
  queue.swap(task_queue_);

  for (auto& task : queue)
    ExecuteTask(task);

  // We might be detached now.
}

HTMLConstructionSite::HTMLConstructionSite(
    HTMLParserReentryPermit* reentry_permit,
    Document& document,
    ParserContentPolicy parser_content_policy)
    : reentry_permit_(reentry_permit),
      document_(&document),
      attachment_root_(document),
      parser_content_policy_(parser_content_policy),
      is_parsing_fragment_(false),
      redirect_attach_to_foster_parent_(false),
      in_quirks_mode_(document.InQuirksMode()) {
  DCHECK(document_->IsHTMLDocument() || document_->IsXHTMLDocument());
}

void HTMLConstructionSite::InitFragmentParsing(DocumentFragment* fragment,
                                               Element* context_element) {
  DCHECK(context_element);
  DCHECK_EQ(document_, &fragment->GetDocument());
  DCHECK_EQ(in_quirks_mode_, fragment->GetDocument().InQuirksMode());
  DCHECK(!is_parsing_fragment_);
  DCHECK(!form_);

  attachment_root_ = fragment;
  is_parsing_fragment_ = true;

  if (!context_element->GetDocument().IsTemplateDocument())
    form_ = Traversal<HTMLFormElement>::FirstAncestorOrSelf(*context_element);
}

HTMLConstructionSite::~HTMLConstructionSite() {
  // Depending on why we're being destroyed it might be OK to forget queued
  // tasks, but currently we don't expect to.
  DCHECK(task_queue_.IsEmpty());
  // Currently we assume that text will never be the last token in the document
  // and that we'll always queue some additional task to cause it to flush.
  DCHECK(pending_text_.IsEmpty());
}

DEFINE_TRACE(HTMLConstructionSite) {
  visitor->Trace(document_);
  visitor->Trace(attachment_root_);
  visitor->Trace(head_);
  visitor->Trace(form_);
  visitor->Trace(open_elements_);
  visitor->Trace(active_formatting_elements_);
  visitor->Trace(task_queue_);
  visitor->Trace(pending_text_);
}

void HTMLConstructionSite::Detach() {
  // FIXME: We'd like to ASSERT here that we're canceling and not just
  // discarding text that really should have made it into the DOM earlier, but
  // there doesn't seem to be a nice way to do that.
  pending_text_.Discard();
  document_ = nullptr;
  attachment_root_ = nullptr;
}

HTMLFormElement* HTMLConstructionSite::TakeForm() {
  return form_.Release();
}

void HTMLConstructionSite::InsertHTMLHtmlStartTagBeforeHTML(
    AtomicHTMLToken* token) {
  DCHECK(document_);
  HTMLHtmlElement* element = HTMLHtmlElement::Create(*document_);
  SetAttributes(element, token, parser_content_policy_);
  AttachLater(attachment_root_, element);
  open_elements_.PushHTMLHtmlElement(HTMLStackItem::Create(element, token));

  ExecuteQueuedTasks();
  element->InsertedByParser();
}

void HTMLConstructionSite::MergeAttributesFromTokenIntoElement(
    AtomicHTMLToken* token,
    Element* element) {
  if (token->Attributes().IsEmpty())
    return;

  for (const auto& token_attribute : token->Attributes()) {
    if (element->AttributesWithoutUpdate().FindIndex(
            token_attribute.GetName()) == kNotFound)
      element->setAttribute(token_attribute.GetName(), token_attribute.Value());
  }
}

void HTMLConstructionSite::InsertHTMLHtmlStartTagInBody(
    AtomicHTMLToken* token) {
  // Fragments do not have a root HTML element, so any additional HTML elements
  // encountered during fragment parsing should be ignored.
  if (is_parsing_fragment_)
    return;

  MergeAttributesFromTokenIntoElement(token, open_elements_.HtmlElement());
}

void HTMLConstructionSite::InsertHTMLBodyStartTagInBody(
    AtomicHTMLToken* token) {
  MergeAttributesFromTokenIntoElement(token, open_elements_.BodyElement());
}

void HTMLConstructionSite::SetDefaultCompatibilityMode() {
  if (is_parsing_fragment_)
    return;
  SetCompatibilityMode(Document::kQuirksMode);
}

void HTMLConstructionSite::SetCompatibilityMode(
    Document::CompatibilityMode mode) {
  in_quirks_mode_ = (mode == Document::kQuirksMode);
  document_->SetCompatibilityMode(mode);
}

void HTMLConstructionSite::SetCompatibilityModeFromDoctype(
    const String& name,
    const String& public_id,
    const String& system_id) {
  // There are three possible compatibility modes:
  // Quirks - quirks mode emulates WinIE and NS4. CSS parsing is also relaxed in
  // this mode, e.g., unit types can be omitted from numbers.
  // Limited Quirks - This mode is identical to no-quirks mode except for its
  // treatment of line-height in the inline box model.
  // No Quirks - no quirks apply. Web pages will obey the specifications to the
  // letter.

  // Check for Quirks Mode.
  if (name != "html" ||
      public_id.StartsWithIgnoringASCIICase(
          "+//Silmaril//dtd html Pro v0r11 19970101//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//AS//DTD HTML 3.0 asWedit + extensions//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//IETF//DTD HTML 2.0 Level 1//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//IETF//DTD HTML 2.0 Level 2//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//IETF//DTD HTML 2.0 Strict Level 1//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//IETF//DTD HTML 2.0 Strict Level 2//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML 2.0 Strict//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML 2.0//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML 2.1E//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML 3.0//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML 3.2 Final//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML 3.2//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML 3//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML Level 0//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML Level 1//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML Level 2//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML Level 3//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//IETF//DTD HTML Strict Level 0//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//IETF//DTD HTML Strict Level 1//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//IETF//DTD HTML Strict Level 2//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//IETF//DTD HTML Strict Level 3//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML Strict//") ||
      public_id.StartsWithIgnoringASCIICase("-//IETF//DTD HTML//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Metrius//DTD Metrius Presentational//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Microsoft//DTD Internet Explorer 2.0 HTML//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Microsoft//DTD Internet Explorer 2.0 Tables//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Microsoft//DTD Internet Explorer 3.0 HTML//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Microsoft//DTD Internet Explorer 3.0 Tables//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Netscape Comm. Corp.//DTD HTML//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Netscape Comm. Corp.//DTD Strict HTML//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//O'Reilly and Associates//DTD HTML 2.0//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//O'Reilly and Associates//DTD HTML Extended 1.0//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//SoftQuad Software//DTD HoTMetaL PRO "
          "6.0::19990601::extensions to HTML 4.0//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//SoftQuad//DTD HoTMetaL PRO "
          "4.0::19971010::extensions to HTML 4.0//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Spyglass//DTD HTML 2.0 Extended//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Sun Microsystems Corp.//DTD HotJava HTML//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//W3C//DTD HTML 3 1995-03-24//") ||
      public_id.StartsWithIgnoringASCIICase("-//W3C//DTD HTML 3.2 Draft//") ||
      public_id.StartsWithIgnoringASCIICase("-//W3C//DTD HTML 3.2 Final//") ||
      public_id.StartsWithIgnoringASCIICase("-//W3C//DTD HTML 3.2//") ||
      public_id.StartsWithIgnoringASCIICase("-//W3C//DTD HTML 3.2S Draft//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//W3C//DTD HTML 4.0 Frameset//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//W3C//DTD HTML 4.0 Transitional//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//W3C//DTD HTML Experimental 19960712//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//W3C//DTD HTML Experimental 970421//") ||
      public_id.StartsWithIgnoringASCIICase("-//W3C//DTD W3 HTML//") ||
      public_id.StartsWithIgnoringASCIICase("-//W3O//DTD W3 HTML 3.0//") ||
      DeprecatedEqualIgnoringCase(public_id,
                                  "-//W3O//DTD W3 HTML Strict 3.0//EN//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//WebTechs//DTD Mozilla HTML 2.0//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//WebTechs//DTD Mozilla HTML//") ||
      DeprecatedEqualIgnoringCase(public_id,
                                  "-/W3C/DTD HTML 4.0 Transitional/EN") ||
      DeprecatedEqualIgnoringCase(public_id, "HTML") ||
      DeprecatedEqualIgnoringCase(
          system_id,
          "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd") ||
      (system_id.IsEmpty() && public_id.StartsWithIgnoringASCIICase(
                                  "-//W3C//DTD HTML 4.01 Frameset//")) ||
      (system_id.IsEmpty() && public_id.StartsWithIgnoringASCIICase(
                                  "-//W3C//DTD HTML 4.01 Transitional//"))) {
    SetCompatibilityMode(Document::kQuirksMode);
    return;
  }

  // Check for Limited Quirks Mode.
  if (public_id.StartsWithIgnoringASCIICase(
          "-//W3C//DTD XHTML 1.0 Frameset//") ||
      public_id.StartsWithIgnoringASCIICase(
          "-//W3C//DTD XHTML 1.0 Transitional//") ||
      (!system_id.IsEmpty() && public_id.StartsWithIgnoringASCIICase(
                                   "-//W3C//DTD HTML 4.01 Frameset//")) ||
      (!system_id.IsEmpty() && public_id.StartsWithIgnoringASCIICase(
                                   "-//W3C//DTD HTML 4.01 Transitional//"))) {
    SetCompatibilityMode(Document::kLimitedQuirksMode);
    return;
  }

  // Otherwise we are No Quirks Mode.
  SetCompatibilityMode(Document::kNoQuirksMode);
}

void HTMLConstructionSite::ProcessEndOfFile() {
  DCHECK(CurrentNode());
  Flush(kFlushAlways);
  OpenElements()->PopAll();
}

void HTMLConstructionSite::FinishedParsing() {
  // We shouldn't have any queued tasks but we might have pending text which we
  // need to promote to tasks and execute.
  DCHECK(task_queue_.IsEmpty());
  Flush(kFlushAlways);
  document_->FinishedParsing();
}

void HTMLConstructionSite::InsertDoctype(AtomicHTMLToken* token) {
  DCHECK_EQ(token->GetType(), HTMLToken::DOCTYPE);

  const String& public_id =
      StringImpl::Create8BitIfPossible(token->PublicIdentifier());
  const String& system_id =
      StringImpl::Create8BitIfPossible(token->SystemIdentifier());
  DocumentType* doctype =
      DocumentType::Create(document_, token->GetName(), public_id, system_id);
  AttachLater(attachment_root_, doctype);

  // DOCTYPE nodes are only processed when parsing fragments w/o
  // contextElements, which never occurs.  However, if we ever chose to support
  // such, this code is subtly wrong, because context-less fragments can
  // determine their own quirks mode, and thus change parsing rules (like <p>
  // inside <table>).  For now we ASSERT that we never hit this code in a
  // fragment, as changing the owning document's compatibility mode would be
  // wrong.
  DCHECK(!is_parsing_fragment_);
  if (is_parsing_fragment_)
    return;

  if (token->ForceQuirks())
    SetCompatibilityMode(Document::kQuirksMode);
  else {
    SetCompatibilityModeFromDoctype(token->GetName(), public_id, system_id);
  }
}

void HTMLConstructionSite::InsertComment(AtomicHTMLToken* token) {
  DCHECK_EQ(token->GetType(), HTMLToken::kComment);
  AttachLater(CurrentNode(),
              Comment::Create(OwnerDocumentForCurrentNode(), token->Comment()));
}

void HTMLConstructionSite::InsertCommentOnDocument(AtomicHTMLToken* token) {
  DCHECK_EQ(token->GetType(), HTMLToken::kComment);
  DCHECK(document_);
  AttachLater(attachment_root_, Comment::Create(*document_, token->Comment()));
}

void HTMLConstructionSite::InsertCommentOnHTMLHtmlElement(
    AtomicHTMLToken* token) {
  DCHECK_EQ(token->GetType(), HTMLToken::kComment);
  ContainerNode* parent = open_elements_.RootNode();
  AttachLater(parent, Comment::Create(parent->GetDocument(), token->Comment()));
}

void HTMLConstructionSite::InsertHTMLHeadElement(AtomicHTMLToken* token) {
  DCHECK(!ShouldFosterParent());
  head_ = HTMLStackItem::Create(CreateElement(token, xhtmlNamespaceURI), token);
  AttachLater(CurrentNode(), head_->GetElement());
  open_elements_.PushHTMLHeadElement(head_);
}

void HTMLConstructionSite::InsertHTMLBodyElement(AtomicHTMLToken* token) {
  DCHECK(!ShouldFosterParent());
  Element* body = CreateElement(token, xhtmlNamespaceURI);
  AttachLater(CurrentNode(), body);
  open_elements_.PushHTMLBodyElement(HTMLStackItem::Create(body, token));
  if (document_)
    document_->WillInsertBody();
}

void HTMLConstructionSite::InsertHTMLFormElement(AtomicHTMLToken* token,
                                                 bool is_demoted) {
  auto* form_element =
      toHTMLFormElement(CreateElement(token, xhtmlNamespaceURI));
  if (!OpenElements()->HasTemplateInHTMLScope())
    form_ = form_element;
  form_element->SetDemoted(is_demoted);
  AttachLater(CurrentNode(), form_element);
  open_elements_.Push(HTMLStackItem::Create(form_element, token));
}

void HTMLConstructionSite::InsertHTMLElement(AtomicHTMLToken* token) {
  Element* element = CreateElement(token, xhtmlNamespaceURI);
  AttachLater(CurrentNode(), element);
  open_elements_.Push(HTMLStackItem::Create(element, token));
}

void HTMLConstructionSite::InsertSelfClosingHTMLElementDestroyingToken(
    AtomicHTMLToken* token) {
  DCHECK_EQ(token->GetType(), HTMLToken::kStartTag);
  // Normally HTMLElementStack is responsible for calling finishParsingChildren,
  // but self-closing elements are never in the element stack so the stack
  // doesn't get a chance to tell them that we're done parsing their children.
  AttachLater(CurrentNode(), CreateElement(token, xhtmlNamespaceURI), true);
  // FIXME: Do we want to acknowledge the token's self-closing flag?
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/tokenization.html#acknowledge-self-closing-flag
}

void HTMLConstructionSite::InsertFormattingElement(AtomicHTMLToken* token) {
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/parsing.html#the-stack-of-open-elements
  // Possible active formatting elements include:
  // a, b, big, code, em, font, i, nobr, s, small, strike, strong, tt, and u.
  InsertHTMLElement(token);
  active_formatting_elements_.Append(CurrentElementRecord()->StackItem());
}

void HTMLConstructionSite::InsertScriptElement(AtomicHTMLToken* token) {
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/scripting-1.html#already-started
  // http://html5.org/specs/dom-parsing.html#dom-range-createcontextualfragment
  // For createContextualFragment, the specifications say to mark it
  // parser-inserted and already-started and later unmark them. However, we
  // short circuit that logic to avoid the subtree traversal to find script
  // elements since scripts can never see those flags or effects thereof.
  const bool parser_inserted = parser_content_policy_ !=
                               kAllowScriptingContentAndDoNotMarkAlreadyStarted;
  const bool already_started = is_parsing_fragment_ && parser_inserted;
  // TODO(csharrison): This logic only works if the tokenizer/parser was not
  // blocked waiting for scripts when the element was inserted. This usually
  // fails for instance, on second document.write if a script writes twice in a
  // row. To fix this, the parser might have to keep track of raw string
  // position.
  // TODO(csharrison): Refactor this so that the bools that are passed
  // in are packed in a bitfield from an enum class.
  const bool created_during_document_write =
      OwnerDocumentForCurrentNode().IsInDocumentWrite();
  HTMLScriptElement* element =
      HTMLScriptElement::Create(OwnerDocumentForCurrentNode(), parser_inserted,
                                already_started, created_during_document_write);
  SetAttributes(element, token, parser_content_policy_);
  if (ScriptingContentIsAllowed(parser_content_policy_))
    AttachLater(CurrentNode(), element);
  open_elements_.Push(HTMLStackItem::Create(element, token));
}

void HTMLConstructionSite::InsertForeignElement(
    AtomicHTMLToken* token,
    const AtomicString& namespace_uri) {
  DCHECK_EQ(token->GetType(), HTMLToken::kStartTag);
  // parseError when xmlns or xmlns:xlink are wrong.
  DVLOG(1) << "Not implemented.";

  Element* element = CreateElement(token, namespace_uri);
  if (ScriptingContentIsAllowed(parser_content_policy_) ||
      !element->IsScriptElement()) {
    AttachLater(CurrentNode(), element, token->SelfClosing());
  }
  if (!token->SelfClosing())
    open_elements_.Push(HTMLStackItem::Create(element, token, namespace_uri));
}

void HTMLConstructionSite::InsertTextNode(const StringView& string,
                                          WhitespaceMode whitespace_mode) {
  HTMLConstructionSiteTask dummy_task(HTMLConstructionSiteTask::kInsert);
  dummy_task.parent = CurrentNode();

  if (ShouldFosterParent())
    FindFosterSite(dummy_task);

  // FIXME: This probably doesn't need to be done both here and in insert(Task).
  if (isHTMLTemplateElement(*dummy_task.parent))
    dummy_task.parent =
        toHTMLTemplateElement(dummy_task.parent.Get())->content();

  // Unclear when parent != case occurs. Somehow we insert text into two
  // separate nodes while processing the same Token. The nextChild !=
  // dummy.nextChild case occurs whenever foster parenting happened and we hit a
  // new text node "<table>a</table>b" In either case we have to flush the
  // pending text into the task queue before making more.
  if (!pending_text_.IsEmpty() &&
      (pending_text_.parent != dummy_task.parent ||
       pending_text_.next_child != dummy_task.next_child))
    FlushPendingText(kFlushAlways);
  pending_text_.Append(dummy_task.parent, dummy_task.next_child, string,
                       whitespace_mode);
}

void HTMLConstructionSite::Reparent(HTMLElementStack::ElementRecord* new_parent,
                                    HTMLElementStack::ElementRecord* child) {
  HTMLConstructionSiteTask task(HTMLConstructionSiteTask::kReparent);
  task.parent = new_parent->GetNode();
  task.child = child->GetNode();
  QueueTask(task);
}

void HTMLConstructionSite::Reparent(HTMLElementStack::ElementRecord* new_parent,
                                    HTMLStackItem* child) {
  HTMLConstructionSiteTask task(HTMLConstructionSiteTask::kReparent);
  task.parent = new_parent->GetNode();
  task.child = child->GetNode();
  QueueTask(task);
}

void HTMLConstructionSite::InsertAlreadyParsedChild(
    HTMLStackItem* new_parent,
    HTMLElementStack::ElementRecord* child) {
  if (new_parent->CausesFosterParenting()) {
    FosterParent(child->GetNode());
    return;
  }

  HTMLConstructionSiteTask task(
      HTMLConstructionSiteTask::kInsertAlreadyParsedChild);
  task.parent = new_parent->GetNode();
  task.child = child->GetNode();
  QueueTask(task);
}

void HTMLConstructionSite::TakeAllChildren(
    HTMLStackItem* new_parent,
    HTMLElementStack::ElementRecord* old_parent) {
  HTMLConstructionSiteTask task(HTMLConstructionSiteTask::kTakeAllChildren);
  task.parent = new_parent->GetNode();
  task.child = old_parent->GetNode();
  QueueTask(task);
}

CreateElementFlags HTMLConstructionSite::GetCreateElementFlags() const {
  return is_parsing_fragment_ ? kCreatedByFragmentParser : kCreatedByParser;
}

inline Document& HTMLConstructionSite::OwnerDocumentForCurrentNode() {
  if (isHTMLTemplateElement(*CurrentNode()))
    return toHTMLTemplateElement(CurrentElement())->content()->GetDocument();
  return CurrentNode()->GetDocument();
}

// "look up a custom element definition" for a token
// https://html.spec.whatwg.org/#look-up-a-custom-element-definition
CustomElementDefinition* HTMLConstructionSite::LookUpCustomElementDefinition(
    Document& document,
    AtomicHTMLToken* token) {
  // "2. If document does not have a browsing context, return null."
  LocalDOMWindow* window = document.ExecutingWindow();
  if (!window)
    return nullptr;

  // "3. Let registry be document's browsing context's Window's
  // CustomElementRegistry object."
  CustomElementRegistry* registry = window->MaybeCustomElements();
  if (!registry)
    return nullptr;

  const AtomicString& local_name = token->GetName();
  const Attribute* is_attribute = token->GetAttributeItem(HTMLNames::isAttr);
  const AtomicString& name = is_attribute ? is_attribute->Value() : local_name;
  CustomElementDescriptor descriptor(name, local_name);

  // 4.-6.
  return registry->DefinitionFor(descriptor);
}

// "create an element for a token"
// https://html.spec.whatwg.org/multipage/syntax.html#create-an-element-for-the-token
Element* HTMLConstructionSite::CreateElement(
    AtomicHTMLToken* token,
    const AtomicString& namespace_uri) {
  // "1. Let document be intended parent's node document."
  Document& document = OwnerDocumentForCurrentNode();

  // "2. Let local name be the tag name of the token."
  QualifiedName tag_name(g_null_atom, token->GetName(), namespace_uri);
  // "3. Let is be the value of the "is" attribute in the given token ..." etc.
  // "4. Let definition be the result of looking up a custom element ..." etc.
  CustomElementDefinition* definition =
      is_parsing_fragment_ ? nullptr
                           : LookUpCustomElementDefinition(document, token);
  // "5. If definition is non-null and the parser was not originally created
  // for the HTML fragment parsing algorithm, then let will execute script
  // be true."
  bool will_execute_script = definition && !is_parsing_fragment_;

  Element* element;

  if (will_execute_script) {
    // "6.1 Increment the document's throw-on-dynamic-insertion counter."
    ThrowOnDynamicMarkupInsertionCountIncrementer
        throw_on_dynamic_markup_insertions(&document);

    // "6.2 If the JavaScript execution context stack is empty,
    // then perform a microtask checkpoint."

    // TODO(dominicc): This is the way the Blink HTML parser performs
    // checkpoints, but note the spec is different--it talks about the
    // JavaScript stack, not the script nesting level.
    if (0u == reentry_permit_->ScriptNestingLevel())
      Microtask::PerformCheckpoint(V8PerIsolateData::MainThreadIsolate());

    // "6.3 Push a new element queue onto the custom element
    // reactions stack."
    CEReactionsScope reactions;

    // 7.
    element = definition->CreateElementSync(document, tag_name);

    // "8. Append each attribute in the given token to element." We don't use
    // setAttributes here because the custom element constructor may have
    // manipulated attributes.
    for (const auto& attribute : token->Attributes())
      element->setAttribute(attribute.GetName(), attribute.Value());

    // "9. If will execute script is true, then ..." etc. The CEReactionsScope
    // and ThrowOnDynamicMarkupInsertionCountIncrementer destructors implement
    // steps 9.1-3.
  } else {
    element = document.createElement(tag_name, GetCreateElementFlags());
    // Definition for the created element does not exist here and it cannot be
    // custom or failed.
    DCHECK_NE(element->GetCustomElementState(), CustomElementState::kCustom);
    DCHECK_NE(element->GetCustomElementState(), CustomElementState::kFailed);

    // TODO(dominicc): Move these steps so they happen for custom
    // elements as well as built-in elements when customized built in
    // elements are implemented for resettable, listed elements.

    // 10. If element has an xmlns attribute in the XMLNS namespace
    // whose value is not exactly the same as the element's namespace,
    // that is a parse error. Similarly, if element has an xmlns:xlink
    // attribute in the XMLNS namespace whose value is not the XLink
    // Namespace, that is a parse error.

    // TODO(dominicc): Implement step 10 when the HTML parser does
    // something useful with parse errors.

    // 11. If element is a resettable element, invoke its reset
    // algorithm. (This initializes the element's value and
    // checkedness based on the element's attributes.)
    // TODO(dominicc): Implement step 11, resettable elements.

    // 12. If element is a form-associated element, and the form
    // element pointer is not null, and there is no template element
    // on the stack of open elements, ...
    FormAssociated* form_associated_element =
        element->IsHTMLElement()
            ? ToHTMLElement(element)->ToFormAssociatedOrNull()
            : nullptr;
    if (form_associated_element && document.GetFrame() && form_.Get()) {
      // ... and element is either not listed or doesn't have a form
      // attribute, and the intended parent is in the same tree as the
      // element pointed to by the form element pointer, associate
      // element with the form element pointed to by the form element
      // pointer, and suppress the running of the reset the form owner
      // algorithm when the parser subsequently attempts to insert the
      // element.

      // TODO(dominicc): There are many differences to the spec here;
      // some of them are observable:
      //
      // - The HTML spec tracks whether there is a template element on
      //   the stack both for manipulating the form element pointer
      //   and using it here.
      // - FormAssociated::AssociateWith implementations don't do the
      //   "same tree" check; for example
      //   HTMLImageElement::AssociateWith just checks whether the form
      //   is in *a* tree. This check should be done here consistently.
      // - ListedElement is a mixin; add IsListedElement and skip
      //   setting the form for listed attributes with form=. Instead
      //   we set attributes (step 8) out of order, after this step,
      //   to reset the form association.
      form_associated_element->AssociateWith(form_.Get());
    }
    // "8. Append each attribute in the given token to element."
    SetAttributes(element, token, parser_content_policy_);
  }

  return element;
}

HTMLStackItem* HTMLConstructionSite::CreateElementFromSavedToken(
    HTMLStackItem* item) {
  Element* element;
  // NOTE: Moving from item -> token -> item copies the Attribute vector twice!
  AtomicHTMLToken fake_token(HTMLToken::kStartTag, item->LocalName(),
                             item->Attributes());
  element = CreateElement(&fake_token, item->NamespaceURI());
  return HTMLStackItem::Create(element, &fake_token, item->NamespaceURI());
}

bool HTMLConstructionSite::IndexOfFirstUnopenFormattingElement(
    unsigned& first_unopen_element_index) const {
  if (active_formatting_elements_.IsEmpty())
    return false;
  unsigned index = active_formatting_elements_.size();
  do {
    --index;
    const HTMLFormattingElementList::Entry& entry =
        active_formatting_elements_.at(index);
    if (entry.IsMarker() || open_elements_.Contains(entry.GetElement())) {
      first_unopen_element_index = index + 1;
      return first_unopen_element_index < active_formatting_elements_.size();
    }
  } while (index);
  first_unopen_element_index = index;
  return true;
}

void HTMLConstructionSite::ReconstructTheActiveFormattingElements() {
  unsigned first_unopen_element_index;
  if (!IndexOfFirstUnopenFormattingElement(first_unopen_element_index))
    return;

  unsigned unopen_entry_index = first_unopen_element_index;
  DCHECK_LT(unopen_entry_index, active_formatting_elements_.size());
  for (; unopen_entry_index < active_formatting_elements_.size();
       ++unopen_entry_index) {
    HTMLFormattingElementList::Entry& unopened_entry =
        active_formatting_elements_.at(unopen_entry_index);
    HTMLStackItem* reconstructed =
        CreateElementFromSavedToken(unopened_entry.StackItem());
    AttachLater(CurrentNode(), reconstructed->GetNode());
    open_elements_.Push(reconstructed);
    unopened_entry.ReplaceElement(reconstructed);
  }
}

void HTMLConstructionSite::GenerateImpliedEndTagsWithExclusion(
    const AtomicString& tag_name) {
  while (HasImpliedEndTag(CurrentStackItem()) &&
         !CurrentStackItem()->MatchesHTMLTag(tag_name))
    open_elements_.Pop();
}

void HTMLConstructionSite::GenerateImpliedEndTags() {
  while (HasImpliedEndTag(CurrentStackItem()))
    open_elements_.Pop();
}

bool HTMLConstructionSite::InQuirksMode() {
  return in_quirks_mode_;
}

// Adjusts |task| to match the "adjusted insertion location" determined by the
// foster parenting algorithm, laid out as the substeps of step 2 of
// https://html.spec.whatwg.org/#appropriate-place-for-inserting-a-node
void HTMLConstructionSite::FindFosterSite(HTMLConstructionSiteTask& task) {
  // 2.1
  HTMLElementStack::ElementRecord* last_template =
      open_elements_.Topmost(templateTag.LocalName());

  // 2.2
  HTMLElementStack::ElementRecord* last_table =
      open_elements_.Topmost(tableTag.LocalName());

  // 2.3
  if (last_template && (!last_table || last_template->IsAbove(last_table))) {
    task.parent = last_template->GetElement();
    return;
  }

  // 2.4
  if (!last_table) {
    // Fragment case
    task.parent = open_elements_.RootNode();  // DocumentFragment
    return;
  }

  // 2.5
  if (ContainerNode* parent = last_table->GetElement()->parentNode()) {
    task.parent = parent;
    task.next_child = last_table->GetElement();
    return;
  }

  // 2.6, 2.7
  task.parent = last_table->Next()->GetElement();
}

bool HTMLConstructionSite::ShouldFosterParent() const {
  return redirect_attach_to_foster_parent_ &&
         CurrentStackItem()->IsElementNode() &&
         CurrentStackItem()->CausesFosterParenting();
}

void HTMLConstructionSite::FosterParent(Node* node) {
  HTMLConstructionSiteTask task(HTMLConstructionSiteTask::kInsert);
  FindFosterSite(task);
  task.child = node;
  DCHECK(task.parent);
  QueueTask(task);
}

DEFINE_TRACE(HTMLConstructionSite::PendingText) {
  visitor->Trace(parent);
  visitor->Trace(next_child);
}

}  // namespace blink
