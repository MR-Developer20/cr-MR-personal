#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <string_view>

class BookMetadataCache;

class Txt {
 public:
  static bool isTxtOrMd(std::string_view path);
  static bool validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize);
  static void invalidateCache(const std::string& cachePath);
  static bool streamTxtToHtml(const std::string& filepath, Print& out);
  // Maps an href used by a .md book's content (an image path relative to the .md file) to a
  // validated SD path. Returns "" for anything that isn't a JPEG/PNG inside the .md's folder.
  static std::string resolveCompanionResource(const std::string& filepath, const std::string& href);
  // The single spine item every TXT/MD book exposes.
  static constexpr const char* CONTENT_HREF = "content.html";
  static std::string findCompanionCoverImage(const std::string& filepath);
  static bool convertCoverImageToBmp(const std::string& imagePath, const std::string& destBmpPath, int thumbHeight = 0,
                                     bool cropped = false, bool originalThresholds = false, int thumbWidth = 0);
  static bool buildTxtCache(const std::string& filepath, const std::string& cachePath,
                            std::unique_ptr<BookMetadataCache>& bookMetadataCache,
                            const std::string& titleOverride = "");
};
