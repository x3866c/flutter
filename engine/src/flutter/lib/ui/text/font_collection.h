// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_TEXT_FONT_COLLECTION_H_
#define FLUTTER_LIB_UI_TEXT_FONT_COLLECTION_H_

#include <memory>
#include <vector>

#include "flutter/assets/asset_manager.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/memory/ref_ptr.h"
#include "third_party/tonic/typed_data/typed_list.h"
#include "txt/font_collection.h"

namespace flutter {

class FontCollection {
 public:
  FontCollection();

  virtual ~FontCollection();

  std::shared_ptr<txt::FontCollection> GetFontCollection() const;

  void SetupDefaultFontManager(uint32_t font_initialization_data);

  // Virtual for testing.
  virtual void RegisterFonts(
      const std::shared_ptr<AssetManager>& asset_manager);

  void RegisterTestFonts();

  static void LoadFontFromList(Dart_Handle font_data_handle,
                               Dart_Handle callback,
                               const std::string& family_name);

  // Registers the font file at |path| by mapping it rather than copying it
  // into memory. Returns false if the file is not a usable font.
  static bool LoadFontFromFile(const std::string& path,
                               const std::string& family_name);

  // Removes a family registered at runtime. Its memory is released once the
  // paragraphs, pictures and glyph caches that use it are gone.
  static bool UnloadFontFamily(const std::string& family_name);

  // Same as above without the root isolate check, for the C entry points.
  static bool LoadFontFromFileInRootIsolate(const std::string& path,
                                            const std::string& family_name);
  static bool UnloadFontFamilyInRootIsolate(const std::string& family_name);

 private:
  std::shared_ptr<txt::FontCollection> collection_;
  sk_sp<txt::DynamicFontManager> dynamic_font_manager_;

  FML_DISALLOW_COPY_AND_ASSIGN(FontCollection);
};

}  // namespace flutter

#endif  // FLUTTER_LIB_UI_TEXT_FONT_COLLECTION_H_
