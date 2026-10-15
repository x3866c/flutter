// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/text/positioned_text.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

#include "flutter/display_list/dl_paint.h"
#include "flutter/display_list/dl_text_skia.h"
#include "flutter/fml/build_config.h"
#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontArguments.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkFontStyle.h"
#include "third_party/skia/include/core/SkTextBlob.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/modules/skparagraph/include/FontArguments.h"  // nogncheck
#include "third_party/skia/modules/skparagraph/include/FontCollection.h"  // nogncheck
#include "third_party/skia/modules/skshaper/include/SkShaper.h"  // nogncheck
#include "third_party/skia/modules/skshaper/include/SkShaper_harfbuzz.h"  // nogncheck
#include "third_party/skia/modules/skunicode/include/SkUnicode_icu.h"  // nogncheck
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/typed_data/typed_list.h"
#include "txt/font_collection.h"
#include "txt/paragraph.h"
#include "txt/paragraph_builder.h"
#include "txt/paragraph_style.h"
#include "txt/text_style.h"

#if IMPELLER_SUPPORTS_RENDERING
#include "flutter/impeller/display_list/dl_text_impeller.h"  // nogncheck
#endif

namespace flutter {

IMPLEMENT_WRAPPERTYPEINFO(ui, PositionedText);

namespace {

namespace skt = skia::textlayout;

// Debug counters (InternalFlutterText_DebugPositionedTextStats).
int64_t g_fallback_lookups = 0;
int64_t g_fallback_micros = 0;
int64_t g_baseline_paragraphs = 0;
int64_t g_baseline_micros = 0;

constexpr SkFourByteTag kWeightAxisTag = SkSetFourByteTag('w', 'g', 'h', 't');

struct ResolvedStyle {
  // Raw values, to build the reference paragraph (BaselineFor).
  std::vector<std::string> family_names;
  int weight = 400;
  bool italic = false;
  std::vector<std::pair<std::string, float>> variations;
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
  SkUnichar first = 0;
  sk_sp<SkTypeface> typeface;
  std::vector<SkGlyphID> glyphs;
  std::vector<SkPoint> points;
};

// Collects the glyphs HarfBuzz produces for a short piece of text.
class GlyphCollector : public SkShaper::RunHandler {
 public:
  void beginLine() override {}
  void runInfo(const RunInfo&) override {}
  void commitRunInfo() override {}
  Buffer runBuffer(const RunInfo& info) override {
    size_t start = glyphs.size();
    glyphs.resize(start + info.glyphCount);
    positions_.resize(glyphs.size());
    return {glyphs.data() + start,
            positions_.data() + start,
            nullptr,
            nullptr,
            {0, 0}};
  }
  void commitRunBuffer(const RunInfo&) override {}
  void commitLine() override {}

  std::vector<SkGlyphID> glyphs;

 private:
  std::vector<SkPoint> positions_;
};

size_t EncodeUtf8(SkUnichar c, char out[4]) {
  if (c < 0x80) {
    out[0] = static_cast<char>(c);
    return 1;
  }
  if (c < 0x800) {
    out[0] = static_cast<char>(0xC0 | (c >> 6));
    out[1] = static_cast<char>(0x80 | (c & 0x3F));
    return 2;
  }
  if (c < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (c >> 12));
    out[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (c & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (c >> 18));
  out[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (c & 0x3F));
  return 4;
}

// The glyph a one-character paragraph draws for |unichar| in |typeface|.
// Paragraphs shape text with HarfBuzz, so default features such as `calt`
// and `locl` can replace the cmap glyph (HarmonyOS Sans SC swaps the curly
// quotes this way). Falls back to |cmap_glyph| when shaping does not give
// exactly one glyph. Results are cached per typeface, character and locale.
SkGlyphID ShapedGlyph(const sk_sp<SkTypeface>& typeface,
                      SkUnichar unichar,
                      SkGlyphID cmap_glyph,
                      const SkString& locale) {
  static std::unordered_map<std::string, SkGlyphID> cache;
  std::string key = std::to_string(typeface->uniqueID());
  key.push_back('|');
  key.append(std::to_string(unichar));
  key.push_back('|');
  key.append(locale.c_str());
  auto found = cache.find(key);
  if (found != cache.end()) {
    return found->second;
  }

  static sk_sp<SkUnicode> unicode = SkUnicodes::ICU::Make();
  SkGlyphID result = cmap_glyph;
  std::unique_ptr<SkShaper> shaper =
      unicode ? SkShapers::HB::ShapeDontWrapOrReorder(unicode,
                                                      SkFontMgr::RefEmpty())
              : nullptr;
  if (shaper) {
    char utf8[4];
    size_t length = EncodeUtf8(unichar, utf8);
    SkFont font(typeface, 16);
    SkShaper::TrivialFontRunIterator font_iter(font, length);
    SkShaper::TrivialBiDiRunIterator bidi_iter(0, length);
    std::unique_ptr<SkShaper::ScriptRunIterator> script_iter =
        SkShapers::HB::ScriptRunIterator(utf8, length);
    SkShaper::TrivialLanguageRunIterator language_iter(locale.c_str(), length);
    GlyphCollector collector;
    shaper->shape(utf8, length, font_iter, bidi_iter, *script_iter,
                  language_iter, nullptr, 0, std::numeric_limits<float>::max(),
                  &collector);
    if (collector.glyphs.size() == 1 && collector.glyphs.front() != 0) {
      result = collector.glyphs.front();
    }
  }
  if (cache.size() > 65536) {
    cache.clear();
  }
  cache.emplace(std::move(key), result);
  return result;
}

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
      *glyph = ShapedGlyph(typeface, unichar, id, locale);
      return typeface;
    }
  }
  auto found = style.fallbacks.find(unichar);
  sk_sp<SkTypeface> fallback;
  if (found != style.fallbacks.end()) {
    fallback = found->second;
  } else {
    auto start = std::chrono::steady_clock::now();
    fallback = collection.defaultFallback(unichar, style.families,
                                          style.font_style, locale,
                                          style.font_args);
    g_fallback_lookups++;
    g_fallback_micros += std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    style.fallbacks.emplace(unichar, fallback);
  }
  if (fallback) {
    SkGlyphID id = fallback->unicharToGlyph(unichar);
    if (id != 0) {
      *glyph = ShapedGlyph(fallback, unichar, id, locale);
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

// The baseline offset (from the top of the line box) at which a
// one-character paragraph in |style| draws its glyph, for a
// character that resolves to |typeface|. Measured with a real paragraph built
// the way TextPainter builds one, so that glyphs drawn at box top + baseline
// land exactly where a per-character paragraph would draw them (the
// paragraph's line metrics involve rounding that a formula would miss). All
// characters resolving to the same typeface share the result, so it is cached.
static float BaselineFor(const ResolvedStyle& style,
                  float height,
                  SkUnichar representative,
                  const std::string& locale,
                  SkTypeface* typeface,
                  bool impeller_enabled) {
  static std::unordered_map<std::string, float> cache;
  std::string key;
  key.reserve(128);
  key.append(std::to_string(typeface->uniqueID()));
  key.push_back('|');
  for (const std::string& family : style.family_names) {
    key.append(family);
    key.push_back(',');
  }
  key.append(std::to_string(style.font_size));
  key.push_back('/');
  key.append(std::to_string(style.weight));
  key.push_back(style.italic ? 'i' : 'n');
  key.append(std::to_string(height));
  for (const auto& variation : style.variations) {
    key.append(variation.first);
    key.append(std::to_string(variation.second));
  }
  key.push_back('|');
  key.append(locale);
  auto found = cache.find(key);
  if (found != cache.end()) {
    return found->second;
  }

  auto baseline_start = std::chrono::steady_clock::now();
  txt::TextStyle text_style;
  text_style.font_families = style.family_names;
  text_style.font_size = style.font_size;
  text_style.font_weight = style.weight;
  text_style.font_style =
      style.italic ? txt::FontStyle::italic : txt::FontStyle::normal;
  text_style.height = height;
  text_style.has_height_override = height != 0;
  text_style.locale = locale;
  for (const auto& variation : style.variations) {
    text_style.font_variations.SetAxisValue(variation.first, variation.second);
  }
  txt::ParagraphStyle paragraph_style;
  paragraph_style.font_family =
      style.family_names.empty() ? "" : style.family_names.front();
  paragraph_style.font_size = style.font_size;
  paragraph_style.font_weight = style.weight;
  paragraph_style.font_style = text_style.font_style;
  paragraph_style.height = height;
  paragraph_style.has_height_override = height != 0;
  paragraph_style.locale = locale;
  paragraph_style.max_lines = 1;
  std::unique_ptr<txt::ParagraphBuilder> builder =
      txt::ParagraphBuilder::CreateSkiaBuilder(
          paragraph_style, CurrentFontCollection(), impeller_enabled);
  builder->PushStyle(text_style);
  std::u16string text;
  if (representative > 0xFFFF) {
    SkUnichar v = representative - 0x10000;
    text.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
    text.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
  } else {
    text.push_back(static_cast<char16_t>(representative));
  }
  builder->AddText(text);
  std::unique_ptr<txt::Paragraph> paragraph = builder->Build();
  paragraph->Layout(std::numeric_limits<double>::max());
  // TextLine::buildTextBlob draws the line at the baseline rounded to whole
  // logical pixels; do the same.
  float baseline = SkScalarFloorToScalar(
      static_cast<float>(paragraph->GetAlphabeticBaseline()) + 0.5f);
  g_baseline_paragraphs++;
  g_baseline_micros += std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - baseline_start)
                           .count();
  if (cache.size() > 4096) {
    cache.clear();
  }
  cache.emplace(std::move(key), baseline);
  return baseline;
}

// Resolves glyphs for |text| and groups them by style and typeface into text
// runs. When |heights| is non-null, |positions| hold the top-left corner of
// each glyph's one-line box instead of its baseline origin, and the baseline
// is placed the way a single-line paragraph with that style's height
// multiplier places it (SkParagraph's Run::calculateMetrics, no half leading),
// using the metrics of the typeface the glyph actually resolves to.
int PositionedText::BuildRuns(const std::u16string& text,
                              Dart_Handle positions_handle,
                              Dart_Handle style_indices_handle,
                              Dart_Handle style_ints_handle,
                              Dart_Handle style_floats_handle,
                              const std::vector<std::string>& families,
                              const std::string& locale_name,
                              const std::vector<float>* heights,
                              std::vector<Run>* runs) {
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
    style.weight = weight;
    style.italic = italic;
    for (int i = 0; i < family_count && family_index < families.size(); i++) {
      style.family_names.push_back(families[family_index]);
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
      char tag_name[5] = {static_cast<char>((tag >> 24) & 0xFF),
                          static_cast<char>((tag >> 16) & 0xFF),
                          static_cast<char>((tag >> 8) & 0xFF),
                          static_cast<char>(tag & 0xFF), 0};
      style.variations.emplace_back(tag_name, value);
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
    return 0;
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
      group.first = unichar;
      group.typeface = typeface;
    }
    group.glyphs.push_back(glyph);
    group.points.push_back(
        SkPoint::Make(positions[index * 2], positions[index * 2 + 1]));
  }

  bool impeller_enabled = UIDartState::Current()->IsImpellerEnabled();
  int glyph_count = 0;
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

    if (heights != nullptr) {
      float height = static_cast<size_t>(group.style) < heights->size()
                         ? (*heights)[group.style]
                         : 0;
      float baseline = BaselineFor(style, height, group.first, locale_name,
                                   group.typeface.get(), impeller_enabled);
      for (SkPoint& point : group.points) {
        point.fY += baseline;
      }
    }

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
    glyph_count += static_cast<int>(group.glyphs.size());
    runs->push_back({std::move(dl_text), style.color});
  }
  return glyph_count;
}

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
  result->glyph_count_ = BuildRuns(
      text, positions_handle, style_indices_handle, style_ints_handle,
      style_floats_handle, families, locale_name, nullptr, &result->runs_);
}

void PositionedText::PaintRuns(const std::vector<Run>& runs,
                               Canvas* canvas,
                               double x,
                               double y) {
  if (!canvas) {
    return;
  }
  DisplayListBuilder* builder = canvas->builder();
  if (!builder) {
    return;
  }
  DlPaint paint;
  for (const Run& run : runs) {
    paint.setColor(run.color);
    builder->DrawText(run.text, static_cast<DlScalar>(x),
                      static_cast<DlScalar>(y), paint);
  }
}

PositionedText::PositionedText() = default;

PositionedText::~PositionedText() = default;

void PositionedText::paint(Canvas* canvas, double x, double y) {
  PaintRuns(runs_, canvas, x, y);
}

void PositionedText::dispose() {
  runs_.clear();
  Dart_EnterScope();
  ClearDartWrapper();
  Dart_ExitScope();
}

}  // namespace flutter

// C entry point for apps that call it through dart:ffi (with Handle
// arguments), so that they keep compiling against engines without
// PositionedText. Draws immediately without keeping native objects around.
// |heights| holds one height multiplier per style; |positions| are the
// top-left corners of the glyph boxes (see PositionedText::BuildRuns). Must be
// called from the root isolate. Returns the number of glyphs drawn, or -1 when
// the call is not possible here.

#if FML_OS_WIN
#define FLUTTER_TEXT_EXPORT __declspec(dllexport)
#else
#define FLUTTER_TEXT_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

FLUTTER_TEXT_EXPORT int64_t InternalFlutterText_DrawPositionedTextBoxes(
    Dart_Handle canvas_handle,
    Dart_Handle text_handle,
    Dart_Handle positions,
    Dart_Handle style_indices,
    Dart_Handle style_ints,
    Dart_Handle style_floats,
    Dart_Handle families_handle,
    Dart_Handle heights_handle,
    Dart_Handle locale_handle,
    double dx,
    double dy) {
  flutter::UIDartState* state = flutter::UIDartState::Current();
  if (!state || !state->IsRootIsolate()) {
    return -1;
  }
  flutter::Canvas* canvas =
      tonic::DartConverter<flutter::Canvas*>::FromDart(canvas_handle);
  if (!canvas) {
    return -1;
  }
  std::u16string text =
      tonic::DartConverter<std::u16string>::FromDart(text_handle);
  std::vector<std::string> families =
      tonic::DartConverter<std::vector<std::string>>::FromDart(
          families_handle);
  tonic::Float32List heights_list(heights_handle);
  std::vector<float> heights(heights_list.data(),
                             heights_list.data() + heights_list.num_elements());
  heights_list.Release();
  std::string locale = tonic::DartConverter<std::string>::FromDart(locale_handle);
  std::vector<flutter::PositionedText::Run> runs;
  int count = flutter::PositionedText::BuildRuns(
      text, positions, style_indices, style_ints, style_floats, families,
      locale, &heights, &runs);
  flutter::PositionedText::PaintRuns(runs, canvas, dx, dy);
  return count;
}

FLUTTER_TEXT_EXPORT void InternalFlutterText_DebugPositionedTextStats(
    int64_t* out) {
  out[0] = flutter::g_fallback_lookups;
  out[1] = flutter::g_fallback_micros;
  out[2] = flutter::g_baseline_paragraphs;
  out[3] = flutter::g_baseline_micros;
}

// Resolves the same glyph runs InternalFlutterText_DrawPositionedTextBoxes
// would, without drawing: font fallback, baselines and the glyph metrics
// behind the text frames are cached, so that a later draw of these
// characters is cheap. Lets callers spread that work over idle frames.
FLUTTER_TEXT_EXPORT int64_t InternalFlutterText_PrewarmPositionedTextBoxes(
    Dart_Handle text_handle,
    Dart_Handle positions,
    Dart_Handle style_indices,
    Dart_Handle style_ints,
    Dart_Handle style_floats,
    Dart_Handle families_handle,
    Dart_Handle heights_handle,
    Dart_Handle locale_handle) {
  flutter::UIDartState* state = flutter::UIDartState::Current();
  if (!state || !state->IsRootIsolate()) {
    return -1;
  }
  std::u16string text =
      tonic::DartConverter<std::u16string>::FromDart(text_handle);
  std::vector<std::string> families =
      tonic::DartConverter<std::vector<std::string>>::FromDart(
          families_handle);
  tonic::Float32List heights_list(heights_handle);
  std::vector<float> heights(heights_list.data(),
                             heights_list.data() + heights_list.num_elements());
  heights_list.Release();
  std::string locale = tonic::DartConverter<std::string>::FromDart(locale_handle);
  std::vector<flutter::PositionedText::Run> runs;
  return flutter::PositionedText::BuildRuns(
      text, positions, style_indices, style_ints, style_floats, families,
      locale, &heights, &runs);
}

}  // extern "C"
