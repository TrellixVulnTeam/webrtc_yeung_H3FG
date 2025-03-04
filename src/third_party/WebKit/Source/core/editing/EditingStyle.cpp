/*
 * Copyright (C) 2007, 2008, 2009 Apple Computer, Inc.
 * Copyright (C) 2010, 2011 Google Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/editing/EditingStyle.h"

#include "bindings/core/v8/ExceptionState.h"
#include "core/HTMLNames.h"
#include "core/css/CSSColorValue.h"
#include "core/css/CSSComputedStyleDeclaration.h"
#include "core/css/CSSIdentifierValue.h"
#include "core/css/CSSPrimitiveValue.h"
#include "core/css/CSSPrimitiveValueMappings.h"
#include "core/css/CSSPropertyMetadata.h"
#include "core/css/CSSRuleList.h"
#include "core/css/CSSStyleRule.h"
#include "core/css/CSSValueList.h"
#include "core/css/FontSize.h"
#include "core/css/StylePropertySet.h"
#include "core/css/StyleRule.h"
#include "core/css/parser/CSSParser.h"
#include "core/css/properties/CSSPropertyAPI.h"
#include "core/css/resolver/StyleResolver.h"
#include "core/dom/Document.h"
#include "core/dom/Element.h"
#include "core/dom/Node.h"
#include "core/dom/NodeComputedStyle.h"
#include "core/dom/NodeTraversal.h"
#include "core/dom/QualifiedName.h"
#include "core/editing/EditingStyleUtilities.h"
#include "core/editing/EditingUtilities.h"
#include "core/editing/Editor.h"
#include "core/editing/FrameSelection.h"
#include "core/editing/Position.h"
#include "core/editing/commands/ApplyStyleCommand.h"
#include "core/editing/serializers/HTMLInterchange.h"
#include "core/frame/LocalFrame.h"
#include "core/html/HTMLFontElement.h"
#include "core/html/HTMLSpanElement.h"
#include "core/layout/LayoutBox.h"
#include "core/layout/LayoutObject.h"
#include "core/style/ComputedStyle.h"
#include "platform/wtf/StdLibExtras.h"

namespace blink {

using namespace cssvalue;

static const CSSPropertyID& TextDecorationPropertyForEditing() {
  static const CSSPropertyID kProperty =
      RuntimeEnabledFeatures::CSS3TextDecorationsEnabled()
          ? CSSPropertyTextDecorationLine
          : CSSPropertyTextDecoration;
  return kProperty;
}

// Editing style properties must be preserved during editing operation.
// e.g. when a user inserts a new paragraph, all properties listed here must be
// copied to the new paragraph.
// NOTE: Use either allEditingProperties() or inheritableEditingProperties() to
// respect runtime enabling of properties.
static const CSSPropertyID kStaticEditingProperties[] = {
    CSSPropertyBackgroundColor, CSSPropertyColor, CSSPropertyFontFamily,
    CSSPropertyFontSize, CSSPropertyFontStyle, CSSPropertyFontVariantLigatures,
    CSSPropertyFontVariantCaps, CSSPropertyFontWeight, CSSPropertyLetterSpacing,
    CSSPropertyOrphans, CSSPropertyTextAlign,
    // FIXME: CSSPropertyTextDecoration needs to be removed when CSS3 Text
    // Decoration feature is no longer experimental.
    CSSPropertyTextDecoration, CSSPropertyTextDecorationLine,
    CSSPropertyTextIndent, CSSPropertyTextTransform, CSSPropertyWhiteSpace,
    CSSPropertyWidows, CSSPropertyWordSpacing,
    CSSPropertyWebkitTextDecorationsInEffect, CSSPropertyWebkitTextFillColor,
    CSSPropertyWebkitTextStrokeColor, CSSPropertyWebkitTextStrokeWidth,
    CSSPropertyCaretColor};

enum EditingPropertiesType {
  kOnlyInheritableEditingProperties,
  kAllEditingProperties
};

static const Vector<CSSPropertyID>& AllEditingProperties() {
  DEFINE_STATIC_LOCAL(Vector<CSSPropertyID>, properties, ());
  if (properties.IsEmpty()) {
    CSSPropertyMetadata::FilterEnabledCSSPropertiesIntoVector(
        kStaticEditingProperties, WTF_ARRAY_LENGTH(kStaticEditingProperties),
        properties);
    if (RuntimeEnabledFeatures::CSS3TextDecorationsEnabled())
      properties.erase(properties.Find(CSSPropertyTextDecoration));
  }
  return properties;
}

static const Vector<CSSPropertyID>& InheritableEditingProperties() {
  DEFINE_STATIC_LOCAL(Vector<CSSPropertyID>, properties, ());
  if (properties.IsEmpty()) {
    CSSPropertyMetadata::FilterEnabledCSSPropertiesIntoVector(
        kStaticEditingProperties, WTF_ARRAY_LENGTH(kStaticEditingProperties),
        properties);
    for (size_t index = 0; index < properties.size();) {
      if (!CSSPropertyAPI::Get(properties[index]).IsInherited()) {
        properties.erase(index);
        continue;
      }
      ++index;
    }
  }
  return properties;
}

template <class StyleDeclarationType>
static MutableStylePropertySet* CopyEditingProperties(
    StyleDeclarationType* style,
    EditingPropertiesType type = kOnlyInheritableEditingProperties) {
  if (type == kAllEditingProperties)
    return style->CopyPropertiesInSet(AllEditingProperties());
  return style->CopyPropertiesInSet(InheritableEditingProperties());
}

static inline bool IsEditingProperty(int id) {
  return AllEditingProperties().Contains(static_cast<CSSPropertyID>(id));
}

static CSSComputedStyleDeclaration* EnsureComputedStyle(
    const Position& position) {
  Element* elem = AssociatedElementOf(position);
  if (!elem)
    return nullptr;
  return CSSComputedStyleDeclaration::Create(elem);
}

static MutableStylePropertySet* GetPropertiesNotIn(
    StylePropertySet* style_with_redundant_properties,
    CSSStyleDeclaration* base_style);
enum LegacyFontSizeMode {
  kAlwaysUseLegacyFontSize,
  kUseLegacyFontSizeOnlyIfPixelValuesMatch
};
static int LegacyFontSizeFromCSSValue(Document*,
                                      const CSSValue*,
                                      bool,
                                      LegacyFontSizeMode);

class HTMLElementEquivalent : public GarbageCollected<HTMLElementEquivalent> {
 public:
  static HTMLElementEquivalent* Create(CSSPropertyID property_id,
                                       CSSValueID primitive_value,
                                       const HTMLQualifiedName& tag_name) {
    return new HTMLElementEquivalent(property_id, primitive_value, tag_name);
  }

  virtual bool Matches(const Element* element) const {
    return !tag_name_ || element->HasTagName(*tag_name_);
  }
  virtual bool HasAttribute() const { return false; }
  virtual bool PropertyExistsInStyle(const StylePropertySet* style) const {
    return style->GetPropertyCSSValue(property_id_);
  }
  virtual bool ValueIsPresentInStyle(HTMLElement*, StylePropertySet*) const;
  virtual void AddToStyle(Element*, EditingStyle*) const;

  DEFINE_INLINE_VIRTUAL_TRACE() { visitor->Trace(identifier_value_); }

 protected:
  HTMLElementEquivalent(CSSPropertyID);
  HTMLElementEquivalent(CSSPropertyID, const HTMLQualifiedName& tag_name);
  HTMLElementEquivalent(CSSPropertyID,
                        CSSValueID primitive_value,
                        const HTMLQualifiedName& tag_name);
  const CSSPropertyID property_id_;
  const Member<CSSIdentifierValue> identifier_value_;
  // We can store a pointer because HTML tag names are const global.
  const HTMLQualifiedName* tag_name_;
};

HTMLElementEquivalent::HTMLElementEquivalent(CSSPropertyID id)
    : property_id_(id), tag_name_(0) {}

HTMLElementEquivalent::HTMLElementEquivalent(CSSPropertyID id,
                                             const HTMLQualifiedName& tag_name)
    : property_id_(id), tag_name_(&tag_name) {}

HTMLElementEquivalent::HTMLElementEquivalent(CSSPropertyID id,
                                             CSSValueID value_id,
                                             const HTMLQualifiedName& tag_name)
    : property_id_(id),
      identifier_value_(CSSIdentifierValue::Create(value_id)),
      tag_name_(&tag_name) {
  DCHECK_NE(value_id, CSSValueInvalid);
}

bool HTMLElementEquivalent::ValueIsPresentInStyle(
    HTMLElement* element,
    StylePropertySet* style) const {
  const CSSValue* value = style->GetPropertyCSSValue(property_id_);

  // TODO: Does this work on style or computed style? The code here, but we
  // might need to do something here to match CSSPrimitiveValues. if
  // (property_id_ == CSSPropertyFontWeight &&
  //     identifier_value_->GetValueID() == CSSValueBold) {
  //   if (value->IsPrimitiveValue() &&
  //       ToCSSPrimitiveValue(value)->GetFloatValue() >= BoldThreshold()) {
  //     LOG(INFO) << "weight match in HTMLElementEquivalent for primitive
  //     value"; return true;
  //   } else {
  //     LOG(INFO) << "weight match in HTMLElementEquivalent for identifier
  //     value";
  //   }
  // }

  return Matches(element) && value && value->IsIdentifierValue() &&
         ToCSSIdentifierValue(value)->GetValueID() ==
             identifier_value_->GetValueID();
}

void HTMLElementEquivalent::AddToStyle(Element*, EditingStyle* style) const {
  style->SetProperty(property_id_, identifier_value_->CssText());
}

class HTMLTextDecorationEquivalent final : public HTMLElementEquivalent {
 public:
  static HTMLElementEquivalent* Create(CSSValueID primitive_value,
                                       const HTMLQualifiedName& tag_name) {
    return new HTMLTextDecorationEquivalent(primitive_value, tag_name);
  }
  bool PropertyExistsInStyle(const StylePropertySet*) const override;
  bool ValueIsPresentInStyle(HTMLElement*, StylePropertySet*) const override;

  DEFINE_INLINE_VIRTUAL_TRACE() { HTMLElementEquivalent::Trace(visitor); }

 private:
  HTMLTextDecorationEquivalent(CSSValueID primitive_value,
                               const HTMLQualifiedName& tag_name);
};

HTMLTextDecorationEquivalent::HTMLTextDecorationEquivalent(
    CSSValueID primitive_value,
    const HTMLQualifiedName& tag_name)
    : HTMLElementEquivalent(TextDecorationPropertyForEditing(),
                            primitive_value,
                            tag_name)
// m_propertyID is used in HTMLElementEquivalent::addToStyle
{}

bool HTMLTextDecorationEquivalent::PropertyExistsInStyle(
    const StylePropertySet* style) const {
  return style->GetPropertyCSSValue(CSSPropertyWebkitTextDecorationsInEffect) ||
         style->GetPropertyCSSValue(TextDecorationPropertyForEditing());
}

bool HTMLTextDecorationEquivalent::ValueIsPresentInStyle(
    HTMLElement* element,
    StylePropertySet* style) const {
  const CSSValue* style_value =
      style->GetPropertyCSSValue(CSSPropertyWebkitTextDecorationsInEffect);
  if (!style_value)
    style_value =
        style->GetPropertyCSSValue(TextDecorationPropertyForEditing());
  return Matches(element) && style_value && style_value->IsValueList() &&
         ToCSSValueList(style_value)->HasValue(*identifier_value_);
}

class HTMLAttributeEquivalent : public HTMLElementEquivalent {
 public:
  static HTMLAttributeEquivalent* Create(CSSPropertyID property_id,
                                         const HTMLQualifiedName& tag_name,
                                         const QualifiedName& attr_name) {
    return new HTMLAttributeEquivalent(property_id, tag_name, attr_name);
  }
  static HTMLAttributeEquivalent* Create(CSSPropertyID property_id,
                                         const QualifiedName& attr_name) {
    return new HTMLAttributeEquivalent(property_id, attr_name);
  }

  bool Matches(const Element* element) const override {
    return HTMLElementEquivalent::Matches(element) &&
           element->hasAttribute(attr_name_);
  }
  bool HasAttribute() const override { return true; }
  bool ValueIsPresentInStyle(HTMLElement*, StylePropertySet*) const override;
  void AddToStyle(Element*, EditingStyle*) const override;
  virtual const CSSValue* AttributeValueAsCSSValue(Element*) const;
  inline const QualifiedName& AttributeName() const { return attr_name_; }

  DEFINE_INLINE_VIRTUAL_TRACE() { HTMLElementEquivalent::Trace(visitor); }

 protected:
  HTMLAttributeEquivalent(CSSPropertyID,
                          const HTMLQualifiedName& tag_name,
                          const QualifiedName& attr_name);
  HTMLAttributeEquivalent(CSSPropertyID, const QualifiedName& attr_name);
  // We can store a reference because HTML attribute names are const global.
  const QualifiedName& attr_name_;
};

HTMLAttributeEquivalent::HTMLAttributeEquivalent(
    CSSPropertyID id,
    const HTMLQualifiedName& tag_name,
    const QualifiedName& attr_name)
    : HTMLElementEquivalent(id, tag_name), attr_name_(attr_name) {}

HTMLAttributeEquivalent::HTMLAttributeEquivalent(CSSPropertyID id,
                                                 const QualifiedName& attr_name)
    : HTMLElementEquivalent(id), attr_name_(attr_name) {}

bool HTMLAttributeEquivalent::ValueIsPresentInStyle(
    HTMLElement* element,
    StylePropertySet* style) const {
  const CSSValue* value = AttributeValueAsCSSValue(element);
  const CSSValue* style_value = style->GetPropertyCSSValue(property_id_);

  return DataEquivalent(value, style_value);
}

void HTMLAttributeEquivalent::AddToStyle(Element* element,
                                         EditingStyle* style) const {
  if (const CSSValue* value = AttributeValueAsCSSValue(element))
    style->SetProperty(property_id_, value->CssText());
}

const CSSValue* HTMLAttributeEquivalent::AttributeValueAsCSSValue(
    Element* element) const {
  DCHECK(element);
  const AtomicString& value = element->getAttribute(attr_name_);
  if (value.IsNull())
    return nullptr;

  MutableStylePropertySet* dummy_style = nullptr;
  dummy_style = MutableStylePropertySet::Create(kHTMLQuirksMode);
  dummy_style->SetProperty(property_id_, value);
  return dummy_style->GetPropertyCSSValue(property_id_);
}

class HTMLFontSizeEquivalent final : public HTMLAttributeEquivalent {
 public:
  static HTMLFontSizeEquivalent* Create() {
    return new HTMLFontSizeEquivalent();
  }
  const CSSValue* AttributeValueAsCSSValue(Element*) const override;

  DEFINE_INLINE_VIRTUAL_TRACE() { HTMLAttributeEquivalent::Trace(visitor); }

 private:
  HTMLFontSizeEquivalent();
};

HTMLFontSizeEquivalent::HTMLFontSizeEquivalent()
    : HTMLAttributeEquivalent(CSSPropertyFontSize,
                              HTMLNames::fontTag,
                              HTMLNames::sizeAttr) {}

const CSSValue* HTMLFontSizeEquivalent::AttributeValueAsCSSValue(
    Element* element) const {
  DCHECK(element);
  const AtomicString& value = element->getAttribute(attr_name_);
  if (value.IsNull())
    return nullptr;
  CSSValueID size;
  if (!HTMLFontElement::CssValueFromFontSizeNumber(value, size))
    return nullptr;
  return CSSIdentifierValue::Create(size);
}

float EditingStyle::no_font_delta_ = 0.0f;

EditingStyle::EditingStyle(ContainerNode* node,
                           PropertiesToInclude properties_to_include) {
  Init(node, properties_to_include);
}

EditingStyle::EditingStyle(const Position& position,
                           PropertiesToInclude properties_to_include) {
  Init(position.AnchorNode(), properties_to_include);
}

EditingStyle::EditingStyle(const StylePropertySet* style)
    : mutable_style_(style ? style->MutableCopy() : nullptr) {
  ExtractFontSizeDelta();
}

EditingStyle::EditingStyle(CSSPropertyID property_id, const String& value)
    : mutable_style_(nullptr) {
  SetProperty(property_id, value);
  is_vertical_align_ = property_id == CSSPropertyVerticalAlign &&
                       (value == "sub" || value == "super");
}

static Color CssValueToColor(const CSSValue* color_value) {
  if (!color_value ||
      (!color_value->IsColorValue() && !color_value->IsPrimitiveValue() &&
       !color_value->IsIdentifierValue()))
    return Color::kTransparent;

  if (color_value->IsColorValue())
    return ToCSSColorValue(color_value)->Value();

  Color color = 0;
  // FIXME: Why ignore the return value?
  CSSParser::ParseColor(color, color_value->CssText());
  return color;
}

static inline Color GetFontColor(CSSStyleDeclaration* style) {
  return CssValueToColor(style->GetPropertyCSSValueInternal(CSSPropertyColor));
}

static inline Color GetFontColor(StylePropertySet* style) {
  return CssValueToColor(style->GetPropertyCSSValue(CSSPropertyColor));
}

static inline Color GetBackgroundColor(CSSStyleDeclaration* style) {
  return CssValueToColor(
      style->GetPropertyCSSValueInternal(CSSPropertyBackgroundColor));
}

static inline Color GetBackgroundColor(StylePropertySet* style) {
  return CssValueToColor(
      style->GetPropertyCSSValue(CSSPropertyBackgroundColor));
}

static inline Color BackgroundColorInEffect(Node* node) {
  return CssValueToColor(
      EditingStyleUtilities::BackgroundColorValueInEffect(node));
}

static int TextAlignResolvingStartAndEnd(int text_align, int direction) {
  switch (text_align) {
    case CSSValueCenter:
    case CSSValueWebkitCenter:
      return CSSValueCenter;
    case CSSValueJustify:
      return CSSValueJustify;
    case CSSValueLeft:
    case CSSValueWebkitLeft:
      return CSSValueLeft;
    case CSSValueRight:
    case CSSValueWebkitRight:
      return CSSValueRight;
    case CSSValueStart:
      return direction != CSSValueRtl ? CSSValueLeft : CSSValueRight;
    case CSSValueEnd:
      return direction == CSSValueRtl ? CSSValueRight : CSSValueLeft;
  }
  return CSSValueInvalid;
}

template <typename T>
static int TextAlignResolvingStartAndEnd(T* style) {
  return TextAlignResolvingStartAndEnd(
      GetIdentifierValue(style, CSSPropertyTextAlign),
      GetIdentifierValue(style, CSSPropertyDirection));
}

void EditingStyle::Init(Node* node, PropertiesToInclude properties_to_include) {
  if (IsTabHTMLSpanElementTextNode(node))
    node = TabSpanElement(node)->parentNode();
  else if (IsTabHTMLSpanElement(node))
    node = node->parentNode();

  CSSComputedStyleDeclaration* computed_style_at_position =
      CSSComputedStyleDeclaration::Create(node);
  mutable_style_ =
      properties_to_include == kAllProperties && computed_style_at_position
          ? computed_style_at_position->CopyProperties()
          : CopyEditingProperties(computed_style_at_position);

  if (properties_to_include == kEditingPropertiesInEffect) {
    if (const CSSValue* value =
            EditingStyleUtilities::BackgroundColorValueInEffect(node))
      mutable_style_->SetProperty(CSSPropertyBackgroundColor, value->CssText());
    if (const CSSValue* value = computed_style_at_position->GetPropertyCSSValue(
            CSSPropertyWebkitTextDecorationsInEffect))
      mutable_style_->SetProperty(CSSPropertyTextDecoration, value->CssText());
  }

  if (node && node->EnsureComputedStyle()) {
    const ComputedStyle* computed_style = node->EnsureComputedStyle();
    RemoveInheritedColorsIfNeeded(computed_style);
    ReplaceFontSizeByKeywordIfPossible(computed_style,
                                       computed_style_at_position);
  }

  is_monospace_font_ = computed_style_at_position->IsMonospaceFont();
  ExtractFontSizeDelta();
}

void EditingStyle::RemoveInheritedColorsIfNeeded(
    const ComputedStyle* computed_style) {
  // If a node's text fill color is currentColor, then its children use
  // their font-color as their text fill color (they don't
  // inherit it).  Likewise for stroke color.
  // Similar thing happens for caret-color if it's auto or currentColor.
  if (computed_style->TextFillColor().IsCurrentColor())
    mutable_style_->RemoveProperty(CSSPropertyWebkitTextFillColor);
  if (computed_style->TextStrokeColor().IsCurrentColor())
    mutable_style_->RemoveProperty(CSSPropertyWebkitTextStrokeColor);
  if (computed_style->CaretColor().IsAutoColor() ||
      computed_style->CaretColor().IsCurrentColor())
    mutable_style_->RemoveProperty(CSSPropertyCaretColor);
}

void EditingStyle::SetProperty(CSSPropertyID property_id,
                               const String& value,
                               bool important) {
  if (!mutable_style_)
    mutable_style_ = MutableStylePropertySet::Create(kHTMLQuirksMode);

  mutable_style_->SetProperty(property_id, value, important);
}

void EditingStyle::ReplaceFontSizeByKeywordIfPossible(
    const ComputedStyle* computed_style,
    CSSComputedStyleDeclaration* css_computed_style) {
  DCHECK(computed_style);
  if (computed_style->GetFontDescription().KeywordSize())
    mutable_style_->SetProperty(
        CSSPropertyFontSize,
        css_computed_style->GetFontSizeCSSValuePreferringKeyword()->CssText());
}

void EditingStyle::ExtractFontSizeDelta() {
  if (!mutable_style_)
    return;

  if (mutable_style_->GetPropertyCSSValue(CSSPropertyFontSize)) {
    // Explicit font size overrides any delta.
    mutable_style_->RemoveProperty(CSSPropertyWebkitFontSizeDelta);
    return;
  }

  // Get the adjustment amount out of the style.
  const CSSValue* value =
      mutable_style_->GetPropertyCSSValue(CSSPropertyWebkitFontSizeDelta);
  if (!value || !value->IsPrimitiveValue())
    return;

  const CSSPrimitiveValue* primitive_value = ToCSSPrimitiveValue(value);

  // Only PX handled now. If we handle more types in the future, perhaps
  // a switch statement here would be more appropriate.
  if (!primitive_value->IsPx())
    return;

  font_size_delta_ = primitive_value->GetFloatValue();
  mutable_style_->RemoveProperty(CSSPropertyWebkitFontSizeDelta);
}

bool EditingStyle::IsEmpty() const {
  return (!mutable_style_ || mutable_style_->IsEmpty()) &&
         font_size_delta_ == no_font_delta_;
}

bool EditingStyle::GetTextDirection(WritingDirection& writing_direction) const {
  if (!mutable_style_)
    return false;

  const CSSValue* unicode_bidi =
      mutable_style_->GetPropertyCSSValue(CSSPropertyUnicodeBidi);
  if (!unicode_bidi || !unicode_bidi->IsIdentifierValue())
    return false;

  CSSValueID unicode_bidi_value =
      ToCSSIdentifierValue(unicode_bidi)->GetValueID();
  if (EditingStyleUtilities::IsEmbedOrIsolate(unicode_bidi_value)) {
    const CSSValue* direction =
        mutable_style_->GetPropertyCSSValue(CSSPropertyDirection);
    if (!direction || !direction->IsIdentifierValue())
      return false;

    writing_direction =
        ToCSSIdentifierValue(direction)->GetValueID() == CSSValueLtr
            ? LeftToRightWritingDirection
            : RightToLeftWritingDirection;

    return true;
  }

  if (unicode_bidi_value == CSSValueNormal) {
    writing_direction = NaturalWritingDirection;
    return true;
  }

  return false;
}

void EditingStyle::OverrideWithStyle(const StylePropertySet* style) {
  if (!style || style->IsEmpty())
    return;
  if (!mutable_style_)
    mutable_style_ = MutableStylePropertySet::Create(kHTMLQuirksMode);
  mutable_style_->MergeAndOverrideOnConflict(style);
  ExtractFontSizeDelta();
}

void EditingStyle::Clear() {
  mutable_style_.Clear();
  is_monospace_font_ = false;
  font_size_delta_ = no_font_delta_;
}

EditingStyle* EditingStyle::Copy() const {
  EditingStyle* copy = EditingStyle::Create();
  if (mutable_style_)
    copy->mutable_style_ = mutable_style_->MutableCopy();
  copy->is_monospace_font_ = is_monospace_font_;
  copy->font_size_delta_ = font_size_delta_;
  return copy;
}

// This is the list of CSS properties that apply specially to block-level
// elements.
static const CSSPropertyID kStaticBlockProperties[] = {
    CSSPropertyBreakAfter,
    CSSPropertyBreakBefore,
    CSSPropertyBreakInside,
    CSSPropertyOrphans,
    CSSPropertyOverflow,  // This can be also be applied to replaced elements
    CSSPropertyColumnCount,
    CSSPropertyColumnGap,
    CSSPropertyColumnRuleColor,
    CSSPropertyColumnRuleStyle,
    CSSPropertyColumnRuleWidth,
    CSSPropertyWebkitColumnBreakBefore,
    CSSPropertyWebkitColumnBreakAfter,
    CSSPropertyWebkitColumnBreakInside,
    CSSPropertyColumnWidth,
    CSSPropertyPageBreakAfter,
    CSSPropertyPageBreakBefore,
    CSSPropertyPageBreakInside,
    CSSPropertyTextAlign,
    CSSPropertyTextAlignLast,
    CSSPropertyTextIndent,
    CSSPropertyTextJustify,
    CSSPropertyWidows};

static const Vector<CSSPropertyID>& BlockPropertiesVector() {
  DEFINE_STATIC_LOCAL(Vector<CSSPropertyID>, properties, ());
  if (properties.IsEmpty())
    CSSPropertyMetadata::FilterEnabledCSSPropertiesIntoVector(
        kStaticBlockProperties, WTF_ARRAY_LENGTH(kStaticBlockProperties),
        properties);
  return properties;
}

EditingStyle* EditingStyle::ExtractAndRemoveBlockProperties() {
  EditingStyle* block_properties = EditingStyle::Create();
  if (!mutable_style_)
    return block_properties;

  block_properties->mutable_style_ =
      mutable_style_->CopyPropertiesInSet(BlockPropertiesVector());
  RemoveBlockProperties();

  return block_properties;
}

EditingStyle* EditingStyle::ExtractAndRemoveTextDirection() {
  EditingStyle* text_direction = EditingStyle::Create();
  text_direction->mutable_style_ =
      MutableStylePropertySet::Create(kHTMLQuirksMode);
  text_direction->mutable_style_->SetProperty(
      CSSPropertyUnicodeBidi, CSSValueIsolate,
      mutable_style_->PropertyIsImportant(CSSPropertyUnicodeBidi));
  text_direction->mutable_style_->SetProperty(
      CSSPropertyDirection,
      mutable_style_->GetPropertyValue(CSSPropertyDirection),
      mutable_style_->PropertyIsImportant(CSSPropertyDirection));

  mutable_style_->RemoveProperty(CSSPropertyUnicodeBidi);
  mutable_style_->RemoveProperty(CSSPropertyDirection);

  return text_direction;
}

void EditingStyle::RemoveBlockProperties() {
  if (!mutable_style_)
    return;

  mutable_style_->RemovePropertiesInSet(BlockPropertiesVector().data(),
                                        BlockPropertiesVector().size());
}

void EditingStyle::RemoveStyleAddedByElement(Element* element) {
  if (!element || !element->parentNode())
    return;
  MutableStylePropertySet* parent_style = CopyEditingProperties(
      CSSComputedStyleDeclaration::Create(element->parentNode()),
      kAllEditingProperties);
  MutableStylePropertySet* node_style = CopyEditingProperties(
      CSSComputedStyleDeclaration::Create(element), kAllEditingProperties);
  node_style->RemoveEquivalentProperties(parent_style);
  mutable_style_->RemoveEquivalentProperties(node_style);
}

void EditingStyle::RemoveStyleConflictingWithStyleOfElement(Element* element) {
  if (!element || !element->parentNode() || !mutable_style_)
    return;

  MutableStylePropertySet* parent_style = CopyEditingProperties(
      CSSComputedStyleDeclaration::Create(element->parentNode()),
      kAllEditingProperties);
  MutableStylePropertySet* node_style = CopyEditingProperties(
      CSSComputedStyleDeclaration::Create(element), kAllEditingProperties);
  node_style->RemoveEquivalentProperties(parent_style);

  unsigned property_count = node_style->PropertyCount();
  for (unsigned i = 0; i < property_count; ++i)
    mutable_style_->RemoveProperty(node_style->PropertyAt(i).Id());
}

void EditingStyle::CollapseTextDecorationProperties() {
  if (!mutable_style_)
    return;

  const CSSValue* text_decorations_in_effect =
      mutable_style_->GetPropertyCSSValue(
          CSSPropertyWebkitTextDecorationsInEffect);
  if (!text_decorations_in_effect)
    return;

  if (text_decorations_in_effect->IsValueList())
    mutable_style_->SetProperty(TextDecorationPropertyForEditing(),
                                text_decorations_in_effect->CssText(),
                                mutable_style_->PropertyIsImportant(
                                    TextDecorationPropertyForEditing()));
  else
    mutable_style_->RemoveProperty(TextDecorationPropertyForEditing());
  mutable_style_->RemoveProperty(CSSPropertyWebkitTextDecorationsInEffect);
}

// CSS properties that create a visual difference only when applied to text.
static const CSSPropertyID kTextOnlyProperties[] = {
    // FIXME: CSSPropertyTextDecoration needs to be removed when CSS3 Text
    // Decoration feature is no longer experimental.
    CSSPropertyTextDecoration,
    CSSPropertyTextDecorationLine,
    CSSPropertyWebkitTextDecorationsInEffect,
    CSSPropertyFontStyle,
    CSSPropertyFontWeight,
    CSSPropertyColor,
};

TriState EditingStyle::TriStateOfStyle(EditingStyle* style) const {
  if (!style || !style->mutable_style_)
    return kFalseTriState;
  return TriStateOfStyle(style->mutable_style_->EnsureCSSStyleDeclaration(),
                         kDoNotIgnoreTextOnlyProperties);
}

TriState EditingStyle::TriStateOfStyle(
    CSSStyleDeclaration* style_to_compare,
    ShouldIgnoreTextOnlyProperties should_ignore_text_only_properties) const {
  MutableStylePropertySet* difference =
      GetPropertiesNotIn(mutable_style_.Get(), style_to_compare);

  if (should_ignore_text_only_properties == kIgnoreTextOnlyProperties)
    difference->RemovePropertiesInSet(kTextOnlyProperties,
                                      WTF_ARRAY_LENGTH(kTextOnlyProperties));

  if (difference->IsEmpty())
    return kTrueTriState;
  if (difference->PropertyCount() == mutable_style_->PropertyCount())
    return kFalseTriState;

  return kMixedTriState;
}

TriState EditingStyle::TriStateOfStyle(
    const VisibleSelection& selection) const {
  if (selection.IsNone())
    return kFalseTriState;

  if (selection.IsCaret()) {
    return TriStateOfStyle(
        EditingStyleUtilities::CreateStyleAtSelectionStart(selection));
  }

  TriState state = kFalseTriState;
  bool node_is_start = true;
  for (Node& node : NodeTraversal::StartsAt(*selection.Start().AnchorNode())) {
    if (node.GetLayoutObject() && HasEditableStyle(node)) {
      CSSComputedStyleDeclaration* node_style =
          CSSComputedStyleDeclaration::Create(&node);
      if (node_style) {
        // If the selected element has <sub> or <sup> ancestor element, apply
        // the corresponding style(vertical-align) to it so that
        // document.queryCommandState() works with the style. See bug
        // http://crbug.com/582225.
        if (is_vertical_align_ &&
            GetIdentifierValue(node_style, CSSPropertyVerticalAlign) ==
                CSSValueBaseline) {
          const CSSIdentifierValue* vertical_align = ToCSSIdentifierValue(
              mutable_style_->GetPropertyCSSValue(CSSPropertyVerticalAlign));
          if (EditingStyleUtilities::HasAncestorVerticalAlignStyle(
                  node, vertical_align->GetValueID()))
            node.MutableComputedStyle()->SetVerticalAlign(
                vertical_align->ConvertTo<EVerticalAlign>());
        }

        // Pass EditingStyle::DoNotIgnoreTextOnlyProperties without checking if
        // node.isTextNode() because the node can be an element node. See bug
        // http://crbug.com/584939.
        TriState node_state = TriStateOfStyle(
            node_style, EditingStyle::kDoNotIgnoreTextOnlyProperties);
        if (node_is_start) {
          state = node_state;
          node_is_start = false;
        } else if (state != node_state && node.IsTextNode()) {
          state = kMixedTriState;
          break;
        }
      }
    }
    if (&node == selection.End().AnchorNode())
      break;
  }

  return state;
}

bool EditingStyle::ConflictsWithInlineStyleOfElement(
    HTMLElement* element,
    EditingStyle* extracted_style,
    Vector<CSSPropertyID>* conflicting_properties) const {
  DCHECK(element);
  DCHECK(!conflicting_properties || conflicting_properties->IsEmpty());

  const StylePropertySet* inline_style = element->InlineStyle();
  if (!mutable_style_ || !inline_style)
    return false;

  unsigned property_count = mutable_style_->PropertyCount();
  for (unsigned i = 0; i < property_count; ++i) {
    CSSPropertyID property_id = mutable_style_->PropertyAt(i).Id();

    // We don't override whitespace property of a tab span because that would
    // collapse the tab into a space.
    if (property_id == CSSPropertyWhiteSpace && IsTabHTMLSpanElement(element))
      continue;

    if (property_id == CSSPropertyWebkitTextDecorationsInEffect &&
        inline_style->GetPropertyCSSValue(TextDecorationPropertyForEditing())) {
      if (!conflicting_properties)
        return true;
      conflicting_properties->push_back(CSSPropertyTextDecoration);
      // Because text-decoration expands to text-decoration-line when CSS3
      // Text Decoration is enabled, we also state it as conflicting.
      if (RuntimeEnabledFeatures::CSS3TextDecorationsEnabled())
        conflicting_properties->push_back(CSSPropertyTextDecorationLine);
      if (extracted_style)
        extracted_style->SetProperty(
            TextDecorationPropertyForEditing(),
            inline_style->GetPropertyValue(TextDecorationPropertyForEditing()),
            inline_style->PropertyIsImportant(
                TextDecorationPropertyForEditing()));
      continue;
    }

    if (!inline_style->GetPropertyCSSValue(property_id))
      continue;

    if (property_id == CSSPropertyUnicodeBidi &&
        inline_style->GetPropertyCSSValue(CSSPropertyDirection)) {
      if (!conflicting_properties)
        return true;
      conflicting_properties->push_back(CSSPropertyDirection);
      if (extracted_style)
        extracted_style->SetProperty(
            property_id, inline_style->GetPropertyValue(property_id),
            inline_style->PropertyIsImportant(property_id));
    }

    if (!conflicting_properties)
      return true;

    conflicting_properties->push_back(property_id);

    if (extracted_style)
      extracted_style->SetProperty(
          property_id, inline_style->GetPropertyValue(property_id),
          inline_style->PropertyIsImportant(property_id));
  }

  return conflicting_properties && !conflicting_properties->IsEmpty();
}

static const HeapVector<Member<HTMLElementEquivalent>>&
HtmlElementEquivalents() {
  DEFINE_STATIC_LOCAL(HeapVector<Member<HTMLElementEquivalent>>,
                      html_element_equivalents,
                      (new HeapVector<Member<HTMLElementEquivalent>>));
  if (!html_element_equivalents.size()) {
    html_element_equivalents.push_back(HTMLElementEquivalent::Create(
        CSSPropertyFontWeight, CSSValueBold, HTMLNames::bTag));
    html_element_equivalents.push_back(HTMLElementEquivalent::Create(
        CSSPropertyFontWeight, CSSValueBold, HTMLNames::strongTag));
    html_element_equivalents.push_back(HTMLElementEquivalent::Create(
        CSSPropertyVerticalAlign, CSSValueSub, HTMLNames::subTag));
    html_element_equivalents.push_back(HTMLElementEquivalent::Create(
        CSSPropertyVerticalAlign, CSSValueSuper, HTMLNames::supTag));
    html_element_equivalents.push_back(HTMLElementEquivalent::Create(
        CSSPropertyFontStyle, CSSValueItalic, HTMLNames::iTag));
    html_element_equivalents.push_back(HTMLElementEquivalent::Create(
        CSSPropertyFontStyle, CSSValueItalic, HTMLNames::emTag));

    html_element_equivalents.push_back(HTMLTextDecorationEquivalent::Create(
        CSSValueUnderline, HTMLNames::uTag));
    html_element_equivalents.push_back(HTMLTextDecorationEquivalent::Create(
        CSSValueLineThrough, HTMLNames::sTag));
    html_element_equivalents.push_back(HTMLTextDecorationEquivalent::Create(
        CSSValueLineThrough, HTMLNames::strikeTag));
  }

  return html_element_equivalents;
}

bool EditingStyle::ConflictsWithImplicitStyleOfElement(
    HTMLElement* element,
    EditingStyle* extracted_style,
    ShouldExtractMatchingStyle should_extract_matching_style) const {
  if (!mutable_style_)
    return false;

  const HeapVector<Member<HTMLElementEquivalent>>& html_element_equivalents =
      HtmlElementEquivalents();
  for (size_t i = 0; i < html_element_equivalents.size(); ++i) {
    const HTMLElementEquivalent* equivalent = html_element_equivalents[i].Get();
    if (equivalent->Matches(element) &&
        equivalent->PropertyExistsInStyle(mutable_style_.Get()) &&
        (should_extract_matching_style == kExtractMatchingStyle ||
         !equivalent->ValueIsPresentInStyle(element, mutable_style_.Get()))) {
      if (extracted_style)
        equivalent->AddToStyle(element, extracted_style);
      return true;
    }
  }
  return false;
}

static const HeapVector<Member<HTMLAttributeEquivalent>>&
HtmlAttributeEquivalents() {
  DEFINE_STATIC_LOCAL(HeapVector<Member<HTMLAttributeEquivalent>>,
                      html_attribute_equivalents,
                      (new HeapVector<Member<HTMLAttributeEquivalent>>));
  if (!html_attribute_equivalents.size()) {
    // elementIsStyledSpanOrHTMLEquivalent depends on the fact each
    // HTMLAttriuteEquivalent matches exactly one attribute of exactly one
    // element except dirAttr.
    html_attribute_equivalents.push_back(HTMLAttributeEquivalent::Create(
        CSSPropertyColor, HTMLNames::fontTag, HTMLNames::colorAttr));
    html_attribute_equivalents.push_back(HTMLAttributeEquivalent::Create(
        CSSPropertyFontFamily, HTMLNames::fontTag, HTMLNames::faceAttr));
    html_attribute_equivalents.push_back(HTMLFontSizeEquivalent::Create());

    html_attribute_equivalents.push_back(HTMLAttributeEquivalent::Create(
        CSSPropertyDirection, HTMLNames::dirAttr));
    html_attribute_equivalents.push_back(HTMLAttributeEquivalent::Create(
        CSSPropertyUnicodeBidi, HTMLNames::dirAttr));
  }

  return html_attribute_equivalents;
}

bool EditingStyle::ConflictsWithImplicitStyleOfAttributes(
    HTMLElement* element) const {
  DCHECK(element);
  if (!mutable_style_)
    return false;

  const HeapVector<Member<HTMLAttributeEquivalent>>&
      html_attribute_equivalents = HtmlAttributeEquivalents();
  for (const auto& equivalent : html_attribute_equivalents) {
    if (equivalent->Matches(element) &&
        equivalent->PropertyExistsInStyle(mutable_style_.Get()) &&
        !equivalent->ValueIsPresentInStyle(element, mutable_style_.Get()))
      return true;
  }

  return false;
}

bool EditingStyle::ExtractConflictingImplicitStyleOfAttributes(
    HTMLElement* element,
    ShouldPreserveWritingDirection should_preserve_writing_direction,
    EditingStyle* extracted_style,
    Vector<QualifiedName>& conflicting_attributes,
    ShouldExtractMatchingStyle should_extract_matching_style) const {
  DCHECK(element);
  // HTMLAttributeEquivalent::addToStyle doesn't support unicode-bidi and
  // direction properties
  if (extracted_style)
    DCHECK_EQ(should_preserve_writing_direction, kPreserveWritingDirection);
  if (!mutable_style_)
    return false;

  const HeapVector<Member<HTMLAttributeEquivalent>>&
      html_attribute_equivalents = HtmlAttributeEquivalents();
  bool removed = false;
  for (const auto& attribute : html_attribute_equivalents) {
    const HTMLAttributeEquivalent* equivalent = attribute.Get();

    // unicode-bidi and direction are pushed down separately so don't push down
    // with other styles.
    if (should_preserve_writing_direction == kPreserveWritingDirection &&
        equivalent->AttributeName() == HTMLNames::dirAttr)
      continue;

    if (!equivalent->Matches(element) ||
        !equivalent->PropertyExistsInStyle(mutable_style_.Get()) ||
        (should_extract_matching_style == kDoNotExtractMatchingStyle &&
         equivalent->ValueIsPresentInStyle(element, mutable_style_.Get())))
      continue;

    if (extracted_style)
      equivalent->AddToStyle(element, extracted_style);
    conflicting_attributes.push_back(equivalent->AttributeName());
    removed = true;
  }

  return removed;
}

bool EditingStyle::StyleIsPresentInComputedStyleOfNode(Node* node) const {
  return !mutable_style_ ||
         GetPropertiesNotIn(mutable_style_.Get(),
                            CSSComputedStyleDeclaration::Create(node))
             ->IsEmpty();
}

bool EditingStyle::ElementIsStyledSpanOrHTMLEquivalent(
    const HTMLElement* element) {
  DCHECK(element);
  bool element_is_span_or_element_equivalent = false;
  if (isHTMLSpanElement(*element)) {
    element_is_span_or_element_equivalent = true;
  } else {
    const HeapVector<Member<HTMLElementEquivalent>>& html_element_equivalents =
        HtmlElementEquivalents();
    size_t i;
    for (i = 0; i < html_element_equivalents.size(); ++i) {
      if (html_element_equivalents[i]->Matches(element)) {
        element_is_span_or_element_equivalent = true;
        break;
      }
    }
  }

  AttributeCollection attributes = element->Attributes();
  if (attributes.IsEmpty()) {
    // span, b, etc... without any attributes
    return element_is_span_or_element_equivalent;
  }

  unsigned matched_attributes = 0;
  const HeapVector<Member<HTMLAttributeEquivalent>>&
      html_attribute_equivalents = HtmlAttributeEquivalents();
  for (const auto& equivalent : html_attribute_equivalents) {
    if (equivalent->Matches(element) &&
        equivalent->AttributeName() != HTMLNames::dirAttr)
      matched_attributes++;
  }

  if (!element_is_span_or_element_equivalent && !matched_attributes) {
    // element is not a span, a html element equivalent, or font element.
    return false;
  }

  if (element->hasAttribute(HTMLNames::styleAttr)) {
    if (const StylePropertySet* style = element->InlineStyle()) {
      unsigned property_count = style->PropertyCount();
      for (unsigned i = 0; i < property_count; ++i) {
        if (!IsEditingProperty(style->PropertyAt(i).Id()))
          return false;
      }
    }
    matched_attributes++;
  }

  // font with color attribute, span with style attribute, etc...
  DCHECK_LE(matched_attributes, attributes.size());
  return matched_attributes >= attributes.size();
}

void EditingStyle::PrepareToApplyAt(
    const Position& position,
    ShouldPreserveWritingDirection should_preserve_writing_direction) {
  if (!mutable_style_)
    return;

  // ReplaceSelectionCommand::handleStyleSpans() requires that this function
  // only removes the editing style. If this function was modified in the future
  // to delete all redundant properties, then add a boolean value to indicate
  // which one of editingStyleAtPosition or computedStyle is called.
  EditingStyle* editing_style_at_position =
      EditingStyle::Create(position, kEditingPropertiesInEffect);
  StylePropertySet* style_at_position =
      editing_style_at_position->mutable_style_.Get();

  const CSSValue* unicode_bidi = nullptr;
  const CSSValue* direction = nullptr;
  if (should_preserve_writing_direction == kPreserveWritingDirection) {
    unicode_bidi = mutable_style_->GetPropertyCSSValue(CSSPropertyUnicodeBidi);
    direction = mutable_style_->GetPropertyCSSValue(CSSPropertyDirection);
  }

  mutable_style_->RemoveEquivalentProperties(style_at_position);

  if (TextAlignResolvingStartAndEnd(mutable_style_.Get()) ==
      TextAlignResolvingStartAndEnd(style_at_position))
    mutable_style_->RemoveProperty(CSSPropertyTextAlign);

  if (GetFontColor(mutable_style_.Get()) == GetFontColor(style_at_position))
    mutable_style_->RemoveProperty(CSSPropertyColor);

  if (EditingStyleUtilities::HasTransparentBackgroundColor(
          mutable_style_.Get()) ||
      CssValueToColor(
          mutable_style_->GetPropertyCSSValue(CSSPropertyBackgroundColor)) ==
          BackgroundColorInEffect(position.ComputeContainerNode()))
    mutable_style_->RemoveProperty(CSSPropertyBackgroundColor);

  if (unicode_bidi && unicode_bidi->IsIdentifierValue()) {
    mutable_style_->SetProperty(
        CSSPropertyUnicodeBidi,
        ToCSSIdentifierValue(unicode_bidi)->GetValueID());
    if (direction && direction->IsIdentifierValue()) {
      mutable_style_->SetProperty(
          CSSPropertyDirection, ToCSSIdentifierValue(direction)->GetValueID());
    }
  }
}

void EditingStyle::MergeTypingStyle(Document* document) {
  DCHECK(document);

  EditingStyle* typing_style = document->GetFrame()->GetEditor().TypingStyle();
  if (!typing_style || typing_style == this)
    return;

  MergeStyle(typing_style->Style(), kOverrideValues);
}

void EditingStyle::MergeInlineStyleOfElement(
    HTMLElement* element,
    CSSPropertyOverrideMode mode,
    PropertiesToInclude properties_to_include) {
  DCHECK(element);
  if (!element->InlineStyle())
    return;

  switch (properties_to_include) {
    case kAllProperties:
      MergeStyle(element->InlineStyle(), mode);
      return;
    case kOnlyEditingInheritableProperties:
      MergeStyle(CopyEditingProperties(element->InlineStyle(),
                                       kOnlyInheritableEditingProperties),
                 mode);
      return;
    case kEditingPropertiesInEffect:
      MergeStyle(
          CopyEditingProperties(element->InlineStyle(), kAllEditingProperties),
          mode);
      return;
  }
}

static inline bool ElementMatchesAndPropertyIsNotInInlineStyleDecl(
    const HTMLElementEquivalent* equivalent,
    const Element* element,
    EditingStyle::CSSPropertyOverrideMode mode,
    StylePropertySet* style) {
  return equivalent->Matches(element) &&
         (!element->InlineStyle() ||
          !equivalent->PropertyExistsInStyle(element->InlineStyle())) &&
         (mode == EditingStyle::kOverrideValues ||
          !equivalent->PropertyExistsInStyle(style));
}

static MutableStylePropertySet* ExtractEditingProperties(
    const StylePropertySet* style,
    EditingStyle::PropertiesToInclude properties_to_include) {
  if (!style)
    return nullptr;

  switch (properties_to_include) {
    case EditingStyle::kAllProperties:
    case EditingStyle::kEditingPropertiesInEffect:
      return CopyEditingProperties(style, kAllEditingProperties);
    case EditingStyle::kOnlyEditingInheritableProperties:
      return CopyEditingProperties(style, kOnlyInheritableEditingProperties);
  }

  NOTREACHED();
  return nullptr;
}

void EditingStyle::MergeInlineAndImplicitStyleOfElement(
    Element* element,
    CSSPropertyOverrideMode mode,
    PropertiesToInclude properties_to_include) {
  EditingStyle* style_from_rules = EditingStyle::Create();
  style_from_rules->MergeStyleFromRulesForSerialization(element);

  if (element->InlineStyle())
    style_from_rules->mutable_style_->MergeAndOverrideOnConflict(
        element->InlineStyle());

  style_from_rules->mutable_style_ = ExtractEditingProperties(
      style_from_rules->mutable_style_.Get(), properties_to_include);
  MergeStyle(style_from_rules->mutable_style_.Get(), mode);

  const HeapVector<Member<HTMLElementEquivalent>>& element_equivalents =
      HtmlElementEquivalents();
  for (const auto& equivalent : element_equivalents) {
    if (ElementMatchesAndPropertyIsNotInInlineStyleDecl(
            equivalent.Get(), element, mode, mutable_style_.Get()))
      equivalent->AddToStyle(element, this);
  }

  const HeapVector<Member<HTMLAttributeEquivalent>>& attribute_equivalents =
      HtmlAttributeEquivalents();
  for (const auto& attribute : attribute_equivalents) {
    if (attribute->AttributeName() == HTMLNames::dirAttr)
      continue;  // We don't want to include directionality
    if (ElementMatchesAndPropertyIsNotInInlineStyleDecl(
            attribute.Get(), element, mode, mutable_style_.Get()))
      attribute->AddToStyle(element, this);
  }
}

static const CSSValueList& MergeTextDecorationValues(
    const CSSValueList& merged_value,
    const CSSValueList& value_to_merge) {
  DEFINE_STATIC_LOCAL(CSSIdentifierValue, underline,
                      (CSSIdentifierValue::Create(CSSValueUnderline)));
  DEFINE_STATIC_LOCAL(CSSIdentifierValue, line_through,
                      (CSSIdentifierValue::Create(CSSValueLineThrough)));
  CSSValueList& result = *merged_value.Copy();
  if (value_to_merge.HasValue(underline) && !merged_value.HasValue(underline))
    result.Append(underline);

  if (value_to_merge.HasValue(line_through) &&
      !merged_value.HasValue(line_through))
    result.Append(line_through);

  return result;
}

void EditingStyle::MergeStyle(const StylePropertySet* style,
                              CSSPropertyOverrideMode mode) {
  if (!style)
    return;

  if (!mutable_style_) {
    mutable_style_ = style->MutableCopy();
    return;
  }

  unsigned property_count = style->PropertyCount();
  for (unsigned i = 0; i < property_count; ++i) {
    StylePropertySet::PropertyReference property = style->PropertyAt(i);
    const CSSValue* value = mutable_style_->GetPropertyCSSValue(property.Id());

    // text decorations never override values
    if ((property.Id() == TextDecorationPropertyForEditing() ||
         property.Id() == CSSPropertyWebkitTextDecorationsInEffect) &&
        property.Value().IsValueList() && value) {
      if (value->IsValueList()) {
        const CSSValueList& result = MergeTextDecorationValues(
            *ToCSSValueList(value), ToCSSValueList(property.Value()));
        mutable_style_->SetProperty(property.Id(), result,
                                    property.IsImportant());
        continue;
      }
      // text-decoration: none is equivalent to not having the property
      value = nullptr;
    }

    if (mode == kOverrideValues || (mode == kDoNotOverrideValues && !value))
      mutable_style_->SetProperty(property.ToCSSProperty());
  }
}

static MutableStylePropertySet* StyleFromMatchedRulesForElement(
    Element* element,
    unsigned rules_to_include) {
  MutableStylePropertySet* style =
      MutableStylePropertySet::Create(kHTMLQuirksMode);
  StyleRuleList* matched_rules =
      element->GetDocument().EnsureStyleResolver().StyleRulesForElement(
          element, rules_to_include);
  if (matched_rules) {
    for (unsigned i = 0; i < matched_rules->size(); ++i)
      style->MergeAndOverrideOnConflict(&matched_rules->at(i)->Properties());
  }
  return style;
}

void EditingStyle::MergeStyleFromRules(Element* element) {
  MutableStylePropertySet* style_from_matched_rules =
      StyleFromMatchedRulesForElement(
          element,
          StyleResolver::kAuthorCSSRules | StyleResolver::kCrossOriginCSSRules);
  // Styles from the inline style declaration, held in the variable "style",
  // take precedence over those from matched rules.
  if (mutable_style_)
    style_from_matched_rules->MergeAndOverrideOnConflict(mutable_style_.Get());

  Clear();
  mutable_style_ = style_from_matched_rules;
}

void EditingStyle::MergeStyleFromRulesForSerialization(Element* element) {
  MergeStyleFromRules(element);

  // The property value, if it's a percentage, may not reflect the actual
  // computed value.
  // For example: style="height: 1%; overflow: visible;" in quirksmode
  // FIXME: There are others like this, see <rdar://problem/5195123> Slashdot
  // copy/paste fidelity problem
  CSSComputedStyleDeclaration* computed_style_for_element =
      CSSComputedStyleDeclaration::Create(element);
  MutableStylePropertySet* from_computed_style =
      MutableStylePropertySet::Create(kHTMLQuirksMode);
  {
    unsigned property_count = mutable_style_->PropertyCount();
    for (unsigned i = 0; i < property_count; ++i) {
      StylePropertySet::PropertyReference property =
          mutable_style_->PropertyAt(i);
      const CSSValue& value = property.Value();
      if (!value.IsPrimitiveValue())
        continue;
      if (ToCSSPrimitiveValue(value).IsPercentage()) {
        if (const CSSValue* computed_property_value =
                computed_style_for_element->GetPropertyCSSValue(property.Id()))
          from_computed_style->AddRespectingCascade(
              CSSProperty(property.Id(), *computed_property_value));
      }
    }
  }
  mutable_style_->MergeAndOverrideOnConflict(from_computed_style);
}

static void RemovePropertiesInStyle(
    MutableStylePropertySet* style_to_remove_properties_from,
    StylePropertySet* style) {
  unsigned property_count = style->PropertyCount();
  Vector<CSSPropertyID> properties_to_remove(property_count);
  for (unsigned i = 0; i < property_count; ++i)
    properties_to_remove[i] = style->PropertyAt(i).Id();

  style_to_remove_properties_from->RemovePropertiesInSet(
      properties_to_remove.data(), properties_to_remove.size());
}

void EditingStyle::RemoveStyleFromRulesAndContext(Element* element,
                                                  ContainerNode* context) {
  DCHECK(element);
  if (!mutable_style_)
    return;

  // StyleResolver requires clean style.
  DCHECK_GE(element->GetDocument().Lifecycle().GetState(),
            DocumentLifecycle::kStyleClean);
  DCHECK(element->GetDocument().IsActive());

  // 1. Remove style from matched rules because style remain without repeating
  // it in inline style declaration
  MutableStylePropertySet* style_from_matched_rules =
      StyleFromMatchedRulesForElement(element,
                                      StyleResolver::kAllButEmptyCSSRules);
  if (style_from_matched_rules && !style_from_matched_rules->IsEmpty())
    mutable_style_ = GetPropertiesNotIn(
        mutable_style_.Get(),
        style_from_matched_rules->EnsureCSSStyleDeclaration());

  // 2. Remove style present in context and not overriden by matched rules.
  EditingStyle* computed_style =
      EditingStyle::Create(context, kEditingPropertiesInEffect);
  if (computed_style->mutable_style_) {
    if (!computed_style->mutable_style_->GetPropertyCSSValue(
            CSSPropertyBackgroundColor))
      computed_style->mutable_style_->SetProperty(CSSPropertyBackgroundColor,
                                                  CSSValueTransparent);

    RemovePropertiesInStyle(computed_style->mutable_style_.Get(),
                            style_from_matched_rules);
    mutable_style_ = GetPropertiesNotIn(
        mutable_style_.Get(),
        computed_style->mutable_style_->EnsureCSSStyleDeclaration());
  }

  // 3. If this element is a span and has display: inline or float: none, remove
  // them unless they are overriden by rules. These rules are added by
  // serialization code to wrap text nodes.
  if (IsStyleSpanOrSpanWithOnlyStyleAttribute(element)) {
    if (!style_from_matched_rules->GetPropertyCSSValue(CSSPropertyDisplay) &&
        GetIdentifierValue(mutable_style_.Get(), CSSPropertyDisplay) ==
            CSSValueInline)
      mutable_style_->RemoveProperty(CSSPropertyDisplay);
    if (!style_from_matched_rules->GetPropertyCSSValue(CSSPropertyFloat) &&
        GetIdentifierValue(mutable_style_.Get(), CSSPropertyFloat) ==
            CSSValueNone)
      mutable_style_->RemoveProperty(CSSPropertyFloat);
  }
}

void EditingStyle::RemovePropertiesInElementDefaultStyle(Element* element) {
  if (!mutable_style_ || mutable_style_->IsEmpty())
    return;

  StylePropertySet* default_style = StyleFromMatchedRulesForElement(
      element, StyleResolver::kUAAndUserCSSRules);

  RemovePropertiesInStyle(mutable_style_.Get(), default_style);
}

void EditingStyle::AddAbsolutePositioningFromElement(const Element& element) {
  LayoutRect rect = element.BoundingBox();
  LayoutObject* layout_object = element.GetLayoutObject();

  LayoutUnit x = rect.X();
  LayoutUnit y = rect.Y();
  LayoutUnit width = rect.Width();
  LayoutUnit height = rect.Height();
  if (layout_object && layout_object->IsBox()) {
    LayoutBox* layout_box = ToLayoutBox(layout_object);

    x -= layout_box->MarginLeft();
    y -= layout_box->MarginTop();

    mutable_style_->SetProperty(CSSPropertyBoxSizing, CSSValueBorderBox);
  }

  mutable_style_->SetProperty(CSSPropertyPosition, CSSValueAbsolute);
  mutable_style_->SetProperty(
      CSSPropertyLeft,
      *CSSPrimitiveValue::Create(x, CSSPrimitiveValue::UnitType::kPixels));
  mutable_style_->SetProperty(
      CSSPropertyTop,
      *CSSPrimitiveValue::Create(y, CSSPrimitiveValue::UnitType::kPixels));
  mutable_style_->SetProperty(
      CSSPropertyWidth,
      *CSSPrimitiveValue::Create(width, CSSPrimitiveValue::UnitType::kPixels));
  mutable_style_->SetProperty(
      CSSPropertyHeight,
      *CSSPrimitiveValue::Create(height, CSSPrimitiveValue::UnitType::kPixels));
}

void EditingStyle::ForceInline() {
  if (!mutable_style_)
    mutable_style_ = MutableStylePropertySet::Create(kHTMLQuirksMode);
  const bool kPropertyIsImportant = true;
  mutable_style_->SetProperty(CSSPropertyDisplay, CSSValueInline,
                              kPropertyIsImportant);
}

int EditingStyle::LegacyFontSize(Document* document) const {
  const CSSValue* css_value =
      mutable_style_->GetPropertyCSSValue(CSSPropertyFontSize);
  if (!css_value ||
      !(css_value->IsPrimitiveValue() || css_value->IsIdentifierValue()))
    return 0;
  return LegacyFontSizeFromCSSValue(document, css_value, is_monospace_font_,
                                    kAlwaysUseLegacyFontSize);
}

DEFINE_TRACE(EditingStyle) {
  visitor->Trace(mutable_style_);
}

static void ReconcileTextDecorationProperties(MutableStylePropertySet* style) {
  const CSSValue* text_decorations_in_effect =
      style->GetPropertyCSSValue(CSSPropertyWebkitTextDecorationsInEffect);
  const CSSValue* text_decoration =
      style->GetPropertyCSSValue(TextDecorationPropertyForEditing());
  // "LayoutTests/editing/execCommand/insert-list-and-strikethrough.html" makes
  // both |textDecorationsInEffect| and |textDecoration| non-null.
  if (text_decorations_in_effect) {
    style->SetProperty(TextDecorationPropertyForEditing(),
                       text_decorations_in_effect->CssText());
    style->RemoveProperty(CSSPropertyWebkitTextDecorationsInEffect);
    text_decoration = text_decorations_in_effect;
  }

  // If text-decoration is set to "none", remove the property because we don't
  // want to add redundant "text-decoration: none".
  if (text_decoration && !text_decoration->IsValueList())
    style->RemoveProperty(TextDecorationPropertyForEditing());
}

StyleChange::StyleChange(EditingStyle* style, const Position& position)
    : apply_bold_(false),
      apply_italic_(false),
      apply_underline_(false),
      apply_line_through_(false),
      apply_subscript_(false),
      apply_superscript_(false) {
  Document* document = position.GetDocument();
  if (!style || !style->Style() || !document || !document->GetFrame() ||
      !AssociatedElementOf(position))
    return;

  CSSComputedStyleDeclaration* computed_style = EnsureComputedStyle(position);
  // FIXME: take care of background-color in effect
  MutableStylePropertySet* mutable_style =
      GetPropertiesNotIn(style->Style(), computed_style);
  DCHECK(mutable_style);

  ReconcileTextDecorationProperties(mutable_style);
  if (!document->GetFrame()->GetEditor().ShouldStyleWithCSS())
    ExtractTextStyles(document, mutable_style,
                      computed_style->IsMonospaceFont());

  // Changing the whitespace style in a tab span would collapse the tab into a
  // space.
  if (IsTabHTMLSpanElementTextNode(position.AnchorNode()) ||
      IsTabHTMLSpanElement((position.AnchorNode())))
    mutable_style->RemoveProperty(CSSPropertyWhiteSpace);

  // If unicode-bidi is present in mutableStyle and direction is not, then add
  // direction to mutableStyle.
  // FIXME: Shouldn't this be done in getPropertiesNotIn?
  if (mutable_style->GetPropertyCSSValue(CSSPropertyUnicodeBidi) &&
      !style->Style()->GetPropertyCSSValue(CSSPropertyDirection))
    mutable_style->SetProperty(
        CSSPropertyDirection,
        style->Style()->GetPropertyValue(CSSPropertyDirection));

  // Save the result for later
  css_style_ = mutable_style->AsText().StripWhiteSpace();
}

static void SetTextDecorationProperty(MutableStylePropertySet* style,
                                      const CSSValueList* new_text_decoration,
                                      CSSPropertyID property_id) {
  if (new_text_decoration->length()) {
    style->SetProperty(property_id, new_text_decoration->CssText(),
                       style->PropertyIsImportant(property_id));
  } else {
    // text-decoration: none is redundant since it does not remove any text
    // decorations.
    style->RemoveProperty(property_id);
  }
}

static bool GetPrimitiveValueNumber(StylePropertySet* style,
                                    CSSPropertyID property_id,
                                    float& number) {
  if (!style)
    return false;
  const CSSValue* value = style->GetPropertyCSSValue(property_id);
  if (!value || !value->IsPrimitiveValue())
    return false;
  number = ToCSSPrimitiveValue(value)->GetFloatValue();
  return true;
}

void StyleChange::ExtractTextStyles(Document* document,
                                    MutableStylePropertySet* style,
                                    bool is_monospace_font) {
  DCHECK(style);

  float weight = 0;
  bool is_number =
      GetPrimitiveValueNumber(style, CSSPropertyFontWeight, weight);
  if (GetIdentifierValue(style, CSSPropertyFontWeight) == CSSValueBold ||
      (is_number && weight >= BoldThreshold())) {
    style->RemoveProperty(CSSPropertyFontWeight);
    apply_bold_ = true;
  }

  int font_style = GetIdentifierValue(style, CSSPropertyFontStyle);
  if (font_style == CSSValueItalic || font_style == CSSValueOblique) {
    style->RemoveProperty(CSSPropertyFontStyle);
    apply_italic_ = true;
  }

  // Assuming reconcileTextDecorationProperties has been called, there should
  // not be -webkit-text-decorations-in-effect
  // Furthermore, text-decoration: none has been trimmed so that text-decoration
  // property is always a CSSValueList.
  const CSSValue* text_decoration =
      style->GetPropertyCSSValue(TextDecorationPropertyForEditing());
  if (text_decoration && text_decoration->IsValueList()) {
    DEFINE_STATIC_LOCAL(CSSIdentifierValue, underline,
                        (CSSIdentifierValue::Create(CSSValueUnderline)));
    DEFINE_STATIC_LOCAL(CSSIdentifierValue, line_through,
                        (CSSIdentifierValue::Create(CSSValueLineThrough)));
    CSSValueList* new_text_decoration = ToCSSValueList(text_decoration)->Copy();
    if (new_text_decoration->RemoveAll(underline))
      apply_underline_ = true;
    if (new_text_decoration->RemoveAll(line_through))
      apply_line_through_ = true;

    // If trimTextDecorations, delete underline and line-through
    SetTextDecorationProperty(style, new_text_decoration,
                              TextDecorationPropertyForEditing());
  }

  int vertical_align = GetIdentifierValue(style, CSSPropertyVerticalAlign);
  switch (vertical_align) {
    case CSSValueSub:
      style->RemoveProperty(CSSPropertyVerticalAlign);
      apply_subscript_ = true;
      break;
    case CSSValueSuper:
      style->RemoveProperty(CSSPropertyVerticalAlign);
      apply_superscript_ = true;
      break;
  }

  if (style->GetPropertyCSSValue(CSSPropertyColor)) {
    apply_font_color_ = GetFontColor(style).Serialized();
    style->RemoveProperty(CSSPropertyColor);
  }

  apply_font_face_ = style->GetPropertyValue(CSSPropertyFontFamily);
  // Remove double quotes for Outlook 2007 compatibility. See
  // https://bugs.webkit.org/show_bug.cgi?id=79448
  apply_font_face_.Replace('"', "");
  style->RemoveProperty(CSSPropertyFontFamily);

  if (const CSSValue* font_size =
          style->GetPropertyCSSValue(CSSPropertyFontSize)) {
    if (!font_size->IsPrimitiveValue() && !font_size->IsIdentifierValue()) {
      // Can't make sense of the number. Put no font size.
      style->RemoveProperty(CSSPropertyFontSize);
    } else if (int legacy_font_size = LegacyFontSizeFromCSSValue(
                   document, font_size, is_monospace_font,
                   kUseLegacyFontSizeOnlyIfPixelValuesMatch)) {
      apply_font_size_ = String::Number(legacy_font_size);
      style->RemoveProperty(CSSPropertyFontSize);
    }
  }
}

static void DiffTextDecorations(MutableStylePropertySet* style,
                                CSSPropertyID property_id,
                                const CSSValue* ref_text_decoration) {
  const CSSValue* text_decoration = style->GetPropertyCSSValue(property_id);
  if (!text_decoration || !text_decoration->IsValueList() ||
      !ref_text_decoration || !ref_text_decoration->IsValueList())
    return;

  CSSValueList* new_text_decoration = ToCSSValueList(text_decoration)->Copy();
  const CSSValueList* values_in_ref_text_decoration =
      ToCSSValueList(ref_text_decoration);

  for (size_t i = 0; i < values_in_ref_text_decoration->length(); i++)
    new_text_decoration->RemoveAll(values_in_ref_text_decoration->Item(i));

  SetTextDecorationProperty(style, new_text_decoration, property_id);
}

static bool FontWeightIsBold(const CSSValue* font_weight) {
  if (font_weight->IsIdentifierValue()) {
    // Because b tag can only bold text, there are only two states in plain
    // html: bold and not bold. Collapse all other values to either one of these
    // two states for editing purposes.

    switch (ToCSSIdentifierValue(font_weight)->GetValueID()) {
      case CSSValueNormal:
        return false;
      case CSSValueBold:
        return true;
      default:
        break;
    }
  }

  CHECK(font_weight->IsPrimitiveValue());
  CHECK(ToCSSPrimitiveValue(font_weight)->IsNumber());
  return ToCSSPrimitiveValue(font_weight)->GetFloatValue() >= BoldThreshold();
}

static bool FontWeightNeedsResolving(const CSSValue* font_weight) {
  if (font_weight->IsPrimitiveValue())
    return false;
  if (!font_weight->IsIdentifierValue())
    return true;
  const CSSValueID value = ToCSSIdentifierValue(font_weight)->GetValueID();
  return value == CSSValueLighter || value == CSSValueBolder;
}

MutableStylePropertySet* GetPropertiesNotIn(
    StylePropertySet* style_with_redundant_properties,
    CSSStyleDeclaration* base_style) {
  DCHECK(style_with_redundant_properties);
  DCHECK(base_style);
  MutableStylePropertySet* result =
      style_with_redundant_properties->MutableCopy();

  result->RemoveEquivalentProperties(base_style);

  const CSSValue* base_text_decorations_in_effect =
      base_style->GetPropertyCSSValueInternal(
          CSSPropertyWebkitTextDecorationsInEffect);
  DiffTextDecorations(result, TextDecorationPropertyForEditing(),
                      base_text_decorations_in_effect);
  DiffTextDecorations(result, CSSPropertyWebkitTextDecorationsInEffect,
                      base_text_decorations_in_effect);

  if (const CSSValue* base_font_weight =
          base_style->GetPropertyCSSValueInternal(CSSPropertyFontWeight)) {
    if (const CSSValue* font_weight =
            result->GetPropertyCSSValue(CSSPropertyFontWeight)) {
      if (!FontWeightNeedsResolving(font_weight) &&
          !FontWeightNeedsResolving(base_font_weight) &&
          (FontWeightIsBold(font_weight) == FontWeightIsBold(base_font_weight)))
        result->RemoveProperty(CSSPropertyFontWeight);
    }
  }

  if (base_style->GetPropertyCSSValueInternal(CSSPropertyColor) &&
      GetFontColor(result) == GetFontColor(base_style))
    result->RemoveProperty(CSSPropertyColor);

  if (base_style->GetPropertyCSSValueInternal(CSSPropertyTextAlign) &&
      TextAlignResolvingStartAndEnd(result) ==
          TextAlignResolvingStartAndEnd(base_style))
    result->RemoveProperty(CSSPropertyTextAlign);

  if (base_style->GetPropertyCSSValueInternal(CSSPropertyBackgroundColor) &&
      GetBackgroundColor(result) == GetBackgroundColor(base_style))
    result->RemoveProperty(CSSPropertyBackgroundColor);

  return result;
}

CSSValueID GetIdentifierValue(StylePropertySet* style,
                              CSSPropertyID property_id) {
  if (!style)
    return CSSValueInvalid;
  const CSSValue* value = style->GetPropertyCSSValue(property_id);
  if (!value || !value->IsIdentifierValue())
    return CSSValueInvalid;
  return ToCSSIdentifierValue(value)->GetValueID();
}

CSSValueID GetIdentifierValue(CSSStyleDeclaration* style,
                              CSSPropertyID property_id) {
  if (!style)
    return CSSValueInvalid;
  const CSSValue* value = style->GetPropertyCSSValueInternal(property_id);
  if (!value || !value->IsIdentifierValue())
    return CSSValueInvalid;
  return ToCSSIdentifierValue(value)->GetValueID();
}

int LegacyFontSizeFromCSSValue(Document* document,
                               const CSSValue* value,
                               bool is_monospace_font,
                               LegacyFontSizeMode mode) {
  if (value->IsPrimitiveValue()) {
    const CSSPrimitiveValue& primitive_value = ToCSSPrimitiveValue(*value);
    CSSPrimitiveValue::LengthUnitType length_type;
    if (CSSPrimitiveValue::UnitTypeToLengthUnitType(
            primitive_value.TypeWithCalcResolved(), length_type) &&
        length_type == CSSPrimitiveValue::kUnitTypePixels) {
      double conversion =
          CSSPrimitiveValue::ConversionToCanonicalUnitsScaleFactor(
              primitive_value.TypeWithCalcResolved());
      int pixel_font_size =
          clampTo<int>(primitive_value.GetDoubleValue() * conversion);
      int legacy_font_size = FontSize::LegacyFontSize(document, pixel_font_size,
                                                      is_monospace_font);
      // Use legacy font size only if pixel value matches exactly to that of
      // legacy font size.
      if (mode == kAlwaysUseLegacyFontSize ||
          FontSize::FontSizeForKeyword(document, legacy_font_size,
                                       is_monospace_font) == pixel_font_size)
        return legacy_font_size;

      return 0;
    }
  }

  if (value->IsIdentifierValue()) {
    const CSSIdentifierValue& identifier_value = ToCSSIdentifierValue(*value);
    if (CSSValueXSmall <= identifier_value.GetValueID() &&
        identifier_value.GetValueID() <= CSSValueWebkitXxxLarge)
      return identifier_value.GetValueID() - CSSValueXSmall + 1;
  }

  return 0;
}

}  // namespace blink
