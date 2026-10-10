// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/text/positioned_text.h"

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

#include "flutter/display_list/dl_paint.h"
#include "flutter/display_list/dl_text_skia.h"
#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontArguments.h"
#include "third_party/skia/include/core/SkFontStyle.h"
#include "third_party/skia/include/core/SkTextBlob.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/modules/skparagraph/include/FontArguments.h"  // nogncheck
#include "third_party/skia/modules/skparagraph/include/FontCollection.h"  // nogncheck
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/typed_data/typed_list.h"
#include "txt/font_collection.h"

#if IMPELLER_SUPPORTS_RENDERING
#include "flutter/impeller/display_list/dl_text_impeller.h"  // nogncheck
#endif

namespace flutter {

IMPLEMENT_WRAPPERTYPEINFO(ui, PositionedText);

namespace {

namespace skt = skia::textlayout;

constexpr SkFourByteTag kWeightAxisTag = SkSetFourByteTag('w', 'g', 'h', 't');

struct ResolvedStyle {
  std::vector<SkString> families;
  SkFontStyle font_style;
  float font_size = 14;
  DlColor color;
  std::optional<skt::FontArguments> font_args;
  std::vector<sk_sp<SkTypeface>> typefaces;
  std::unordered_map<SkUnichar, sk_sp<SkTypeface>> fallbacks;
};

struct Group {
  int style = 0;
  sk_sp<SkTypeface> typeface;
  std::vector<SkGlyphID> glyphs;
  std::vector<SkPoint> points;
};

// Mirrors how a paragraph picks a typeface for a code point: the first
// requested family that has the glyph, otherwise the platform fallback,
// otherwise the first family (drawing its missing-glyph box).
sk_sp<SkTypeface> ResolveTypeface(ResolvedStyle& style,
                                  skt::FontCollection& collection,
                                  SkUnichar unichar,
                                  const SkString& locale,
                                  SkGlyphID* glyph) {
  for (const sk_sp<SkTypeface>& typeface : style.typefaces) {
    SkGlyphID id = typeface->unicharToGlyph(unichar);
    if (id != 0) {
      *glyph = id;
      return typeface;
    }
  }
  auto found = style.fallbacks.find(unichar);
  sk_sp<SkTypeface> fallback;
  if (found != style.fallbacks.end()) {
    fallback = found->second;
  } else {
    fallback = collection.defaultFallback(unichar, style.families,
                                          style.font_style, locale,
                                          style.font_args);
    style.fallbacks.emplace(unichar, fallback);
  }
  if (fallback) {
    SkGlyphID id = fallback->unicharToGlyph(unichar);
    if (id != 0) {
      *glyph = id;
      return fallback;
    }
  }
  *glyph = 0;
  return style.typefaces.empty() ? nullptr : style.typefaces.front();
}

std::shared_ptr<txt::FontCollection> CurrentFontCollection() {
  UIDartState* state = UIDartState::Current();
  return state->platform_configuration()
      ->client()
      ->GetFontCollection()
      .GetFontCollection();
}

}  // namespace

void PositionedText::Create(Dart_Handle wrapper,
                            const std::u16string& text,
                            Dart_Handle positions_handle,
                            Dart_Handle style_indices_handle,
                            Dart_Handle style_ints_handle,
                            Dart_Handle style_floats_handle,
                            const std::vector<std::string>& families,
                            const std::string& locale_name) {
  UIDartState::ThrowIfUIOperationsProhibited();
  auto result = fml::MakeRefCounted<PositionedText>();
  result->AssociateWithDartWrapper(wrapper);

  tonic::Float32List positions(positions_handle);
  tonic::Int32List style_indices(style_indices_handle);
  tonic::Int32List style_ints(style_ints_handle);
  tonic::Float32List style_floats(style_floats_handle);

  sk_sp<skt::FontCollection> collection =
      CurrentFontCollection()->CreateSktFontCollection();
  SkString locale(locale_name.c_str());

  // Decode styles.
  std::vector<ResolvedStyle> styles;
  size_t int_index = 0;
  size_t float_index = 0;
  size_t family_index = 0;
  const size_t style_int_count = style_ints.num_elements();
  const size_t style_float_count = style_floats.num_elements();
  const size_t glyph_capacity =
      std::min<size_t>(style_indices.num_elements(),
                       static_cast<size_t>(positions.num_elements()) / 2);
  while (int_index + 5 <= style_int_count && float_index < style_float_count) {
    ResolvedStyle style;
    int weight = style_ints[int_index++];
    bool italic = style_ints[int_index++] != 0;
    style.color = DlColor(static_cast<uint32_t>(style_ints[int_index++]));
    int family_count = style_ints[int_index++];
    int variation_count = style_ints[int_index++];
    style.font_size = style_floats[float_index++];
    style.font_style = SkFontStyle(weight, SkFontStyle::kNormal_Width,
                                   italic ? SkFontStyle::kItalic_Slant
                                          : SkFontStyle::kUpright_Slant);
    for (int i = 0; i < family_count && family_index < families.size(); i++) {
      style.families.emplace_back(families[family_index++].c_str());
    }
    // Like paragraphs, map the weight onto the `wght` axis unless the caller
    // sets that axis.
    std::vector<SkFontArguments::VariationPosition::Coordinate> coordinates;
    bool weight_axis_set = false;
    for (int i = 0; i < variation_count && int_index < style_int_count &&
                    float_index < style_float_count;
         i++) {
      SkFourByteTag tag = static_cast<SkFourByteTag>(style_ints[int_index++]);
      float value = style_floats[float_index++];
      weight_axis_set |= tag == kWeightAxisTag;
      coordinates.push_back({tag, value});
    }
    if (!weight_axis_set) {
      coordinates.push_back({kWeightAxisTag, static_cast<float>(weight)});
    }
    SkFontArguments::VariationPosition position = {
        coordinates.data(), static_cast<int>(coordinates.size())};
    style.font_args = skt::FontArguments(
        SkFontArguments().setVariationDesignPosition(position));
    style.typefaces = collection->findTypefaces(
        style.families, style.font_style, style.font_args);
    styles.push_back(std::move(style));
  }
  if (styles.empty()) {
    return;
  }

  // Resolve glyphs and group them by style and typeface.
  std::map<std::pair<int, SkTypeface*>, Group> groups;
  const uint16_t* cursor = reinterpret_cast<const uint16_t*>(text.data());
  const uint16_t* end = cursor + text.size();
  size_t code_point = 0;
  while (cursor < end) {
    SkUnichar unichar = *cursor++;
    if (unichar >= 0xD800 && unichar <= 0xDBFF && cursor < end &&
        *cursor >= 0xDC00 && *cursor <= 0xDFFF) {
      unichar = 0x10000 + ((unichar - 0xD800) << 10) + (*cursor++ - 0xDC00);
    }
    size_t index = code_point++;
    if (index >= glyph_capacity) {
      break;
    }
    // Whitespace and control characters draw nothing.
    if (unichar <= 0x20 || unichar == 0x7F || unichar == 0x3000) {
      continue;
    }
    int style_index = style_indices[index];
    if (style_index < 0 || static_cast<size_t>(style_index) >= styles.size()) {
      continue;
    }
    SkGlyphID glyph = 0;
    sk_sp<SkTypeface> typeface = ResolveTypeface(
        styles[style_index], *collection, unichar, locale, &glyph);
    if (!typeface) {
      continue;
    }
    Group& group = groups[{style_index, typeface.get()}];
    if (!group.typeface) {
      group.style = style_index;
      group.typeface = typeface;
    }
    group.glyphs.push_back(glyph);
    group.points.push_back(
        SkPoint::Make(positions[index * 2], positions[index * 2 + 1]));
  }

  bool impeller_enabled = UIDartState::Current()->IsImpellerEnabled();
  for (auto& [key, group] : groups) {
    const ResolvedStyle& style = styles[group.style];
    SkFont font(group.typeface, style.font_size);
    // Same font settings as paragraph text.
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setHinting(SkFontHinting::kSlight);
    font.setSubpixel(true);
    int wanted_weight = style.font_style.weight();
    font.setEmbolden(wanted_weight >= SkFontStyle::kSemiBold_Weight &&
                     wanted_weight - group.typeface->fontStyle().weight() >=
                         200);
    bool fake_italic =
        style.font_style.slant() == SkFontStyle::kItalic_Slant &&
        group.typeface->fontStyle().slant() != SkFontStyle::kItalic_Slant;
    font.setSkewX(fake_italic ? -SK_Scalar1 / 4 : 0);

    SkTextBlobBuilder builder;
    const auto& buffer =
        builder.allocRunPos(font, static_cast<int>(group.glyphs.size()));
    std::copy(group.glyphs.begin(), group.glyphs.end(), buffer.glyphs);
    std::copy(group.points.begin(), group.points.end(), buffer.points());
    sk_sp<SkTextBlob> blob = builder.make();
    if (!blob) {
      continue;
    }
    std::shared_ptr<DlText> dl_text;
#if IMPELLER_SUPPORTS_RENDERING
    if (impeller_enabled) {
      dl_text = DlTextImpeller::MakeFromBlob(blob);
    } else {
      dl_text = DlTextSkia::Make(blob);
    }
#else
    dl_text = DlTextSkia::Make(blob);
#endif
    result->glyph_count_ += static_cast<int>(group.glyphs.size());
    result->runs_.push_back({std::move(dl_text), style.color});
  }
}

PositionedText::PositionedText() = default;

PositionedText::~PositionedText() = default;

void PositionedText::paint(Canvas* canvas, double x, double y) {
  if (!canvas) {
    return;
  }
  DisplayListBuilder* builder = canvas->builder();
  if (!builder) {
    return;
  }
  DlPaint paint;
  for (const Run& run : runs_) {
    paint.setColor(run.color);
    builder->DrawText(run.text, static_cast<DlScalar>(x),
                      static_cast<DlScalar>(y), paint);
  }
}

void PositionedText::dispose() {
  runs_.clear();
  ClearDartWrapper();
}

}  // namespace flutter
