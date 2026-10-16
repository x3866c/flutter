// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/typographer/glyph_atlas.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <numeric>
#include <utility>

#include "impeller/typographer/font_glyph_pair.h"

namespace impeller {

namespace {
std::mutex g_coverage_curve_mutex;
std::optional<GlyphCoverageCurve::Table> g_coverage_curve;
std::atomic<uint64_t> g_coverage_curve_generation{0};
}  // namespace

void GlyphCoverageCurve::Set(const uint8_t* table) {
  std::scoped_lock lock(g_coverage_curve_mutex);
  if (table) {
    Table copy;
    std::copy(table, table + copy.size(), copy.begin());
    // Uncovered pixels must stay uncovered, the atlas is not cleared between
    // glyphs.
    copy[0] = 0;
    if (g_coverage_curve.has_value() && g_coverage_curve.value() == copy) {
      return;
    }
    g_coverage_curve = copy;
  } else {
    if (!g_coverage_curve.has_value()) {
      return;
    }
    g_coverage_curve.reset();
  }
  g_coverage_curve_generation.fetch_add(1);
}

uint64_t GlyphCoverageCurve::GetGeneration() {
  return g_coverage_curve_generation.load();
}

std::optional<GlyphCoverageCurve::Table> GlyphCoverageCurve::Get() {
  std::scoped_lock lock(g_coverage_curve_mutex);
  return g_coverage_curve;
}

void GlyphCoverageCurve::Apply(uint8_t* pixels,
                               size_t row_bytes,
                               int width,
                               int height,
                               const Table& table) {
  for (int y = 0; y < height; y++) {
    uint8_t* row = pixels + y * row_bytes;
    for (int x = 0; x < width; x++) {
      row[x] = table[row[x]];
    }
  }
}

GlyphAtlasContext::GlyphAtlasContext(GlyphAtlas::Type type)
    : atlas_(std::make_shared<GlyphAtlas>(type, /*initial_generation=*/0)),
      atlas_size_(ISize(0, 0)) {}

GlyphAtlasContext::~GlyphAtlasContext() {}

std::shared_ptr<GlyphAtlas> GlyphAtlasContext::GetGlyphAtlas() const {
  return atlas_;
}

const ISize& GlyphAtlasContext::GetAtlasSize() const {
  return atlas_size_;
}

int64_t GlyphAtlasContext::GetHeightAdjustment() const {
  return height_adjustment_;
}

std::shared_ptr<RectanglePacker> GlyphAtlasContext::GetRectPacker() const {
  return rect_packer_;
}

void GlyphAtlasContext::UpdateGlyphAtlas(std::shared_ptr<GlyphAtlas> atlas,
                                         ISize size,
                                         int64_t height_adjustment) {
  atlas_ = std::move(atlas);
  atlas_size_ = size;
  height_adjustment_ = height_adjustment;
}

uint64_t GlyphAtlasContext::GetCoverageCurveGeneration() const {
  return coverage_curve_generation_;
}

void GlyphAtlasContext::SetCoverageCurveGeneration(uint64_t generation) {
  coverage_curve_generation_ = generation;
}

void GlyphAtlasContext::UpdateRectPacker(
    std::shared_ptr<RectanglePacker> rect_packer) {
  rect_packer_ = std::move(rect_packer);
}

GlyphAtlas::GlyphAtlas(Type type, size_t initial_generation)
    : type_(type), generation_(initial_generation) {}

GlyphAtlas::~GlyphAtlas() = default;

bool GlyphAtlas::IsValid() const {
  return !!texture_;
}

GlyphAtlas::Type GlyphAtlas::GetType() const {
  return type_;
}

const std::shared_ptr<Texture>& GlyphAtlas::GetTexture() const {
  return texture_;
}

void GlyphAtlas::SetTexture(std::shared_ptr<Texture> texture) {
  texture_ = std::move(texture);
}

size_t GlyphAtlas::GetAtlasGeneration() const {
  return generation_;
}

void GlyphAtlas::SetAtlasGeneration(size_t generation) {
  generation_ = generation;
}

void GlyphAtlas::AddTypefaceGlyphPositionAndBounds(const FontGlyphPair& pair,
                                                   Rect position,
                                                   Rect bounds) {
  FontAtlasMap::iterator it = font_atlas_map_.find(pair.scaled_font);
  FML_DCHECK(it != font_atlas_map_.end());
  it->second.positions_[pair.glyph] =
      FrameBounds{position, bounds, /*is_placeholder=*/false};
}

std::optional<FrameBounds> GlyphAtlas::FindFontGlyphBounds(
    const FontGlyphPair& pair) const {
  const auto& found = font_atlas_map_.find(pair.scaled_font);
  if (found == font_atlas_map_.end()) {
    return std::nullopt;
  }
  return found->second.FindGlyphBounds(pair.glyph);
}

FontGlyphAtlas* GlyphAtlas::GetOrCreateFontGlyphAtlas(
    const ScaledFont& scaled_font) {
  auto [iter, inserted] =
      font_atlas_map_.try_emplace(scaled_font, FontGlyphAtlas());
  return &iter->second;
}

const FontGlyphAtlas* GlyphAtlas::GetFontGlyphAtlas(
    const ScaledFont& scaled_font) const {
  const auto& found = font_atlas_map_.find(scaled_font);
  if (found == font_atlas_map_.end()) {
    return nullptr;
  }
  return &found->second;
}

size_t GlyphAtlas::GetGlyphCount() const {
  return std::accumulate(font_atlas_map_.begin(), font_atlas_map_.end(), 0,
                         [](const int a, const auto& b) {
                           return a + b.second.positions_.size();
                         });
}

size_t GlyphAtlas::IterateGlyphs(
    const std::function<bool(const ScaledFont& scaled_font,
                             const SubpixelGlyph& glyph,
                             const Rect& rect)>& iterator) const {
  if (!iterator) {
    return 0u;
  }

  size_t count = 0u;
  for (const auto& font_value : font_atlas_map_) {
    for (const auto& glyph_value : font_value.second.positions_) {
      count++;
      if (!iterator(font_value.first, glyph_value.first,
                    glyph_value.second.atlas_bounds)) {
        return count;
      }
    }
  }
  return count;
}

std::optional<FrameBounds> FontGlyphAtlas::FindGlyphBounds(
    const SubpixelGlyph& glyph) const {
  const auto& found = positions_.find(glyph);
  if (found == positions_.end()) {
    return std::nullopt;
  }
  return found->second;
}

void FontGlyphAtlas::AppendGlyph(const SubpixelGlyph& glyph,
                                 const FrameBounds& frame_bounds) {
  positions_[glyph] = frame_bounds;
}

}  // namespace impeller
