// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_TEXT_POSITIONED_TEXT_H_
#define FLUTTER_LIB_UI_TEXT_POSITIONED_TEXT_H_

#include <memory>
#include <string>
#include <vector>

#include "flutter/display_list/dl_color.h"
#include "flutter/display_list/dl_text.h"
#include "flutter/lib/ui/dart_wrapper.h"
#include "flutter/lib/ui/painting/canvas.h"

namespace flutter {

// Characters drawn at caller-provided baseline positions.
//
// The typeface for each code point is resolved the way a paragraph resolves
// it (requested families, then platform fallback), but no shaping is done:
// every code point maps to exactly one glyph. Glyphs are grouped by style and
// typeface into text blobs when the object is created, so painting only
// appends one text op per group to the display list.
class PositionedText : public RefCountedDartWrappable<PositionedText> {
  DEFINE_WRAPPERTYPEINFO();
  FML_FRIEND_MAKE_REF_COUNTED(PositionedText);

 public:
  // Per style, |style_ints| holds: weight, slant (0 upright, 1 italic),
  // ARGB color, family count, variation count, then one four-byte tag per
  // variation. |style_floats| holds: font size, then one value per variation.
  // |families| holds the family names of all styles in order.
  // |positions| holds an x, y baseline origin per code point of |text|, and
  // |style_indices| a style index per code point.
  static void Create(Dart_Handle wrapper,
                     const std::u16string& text,
                     Dart_Handle positions,
                     Dart_Handle style_indices,
                     Dart_Handle style_ints,
                     Dart_Handle style_floats,
                     const std::vector<std::string>& families,
                     const std::string& locale);

  ~PositionedText() override;

  int glyphCount() const { return glyph_count_; }
  int runCount() const { return static_cast<int>(runs_.size()); }

  void paint(Canvas* canvas, double x, double y);

  void dispose();

  struct Run {
    std::shared_ptr<DlText> text;
    DlColor color;
  };

  // See positioned_text.cc. Returns the number of glyphs in |runs|.
  static int BuildRuns(const std::u16string& text,
                       Dart_Handle positions,
                       Dart_Handle style_indices,
                       Dart_Handle style_ints,
                       Dart_Handle style_floats,
                       const std::vector<std::string>& families,
                       const std::string& locale,
                       const std::vector<float>* heights,
                       std::vector<Run>* runs);

  static void PaintRuns(const std::vector<Run>& runs,
                        Canvas* canvas,
                        double x,
                        double y);

 private:

  PositionedText();

  std::vector<Run> runs_;
  int glyph_count_ = 0;
};

}  // namespace flutter

#endif  // FLUTTER_LIB_UI_TEXT_POSITIONED_TEXT_H_
