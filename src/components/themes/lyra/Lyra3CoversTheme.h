

#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

namespace Lyra3CoversMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  // 40 px shorter than before so the six-row Home menu (To-do plus a Plugins/OPDS entry) clears the button
  // hints: the menu now starts at 56 + 260 + 16 = 332 and its sixth row ends at 756, above the hints at 760.
  v.homeCoverHeight = LyraMetrics::values.homeCoverHeight - 40;
  v.homeCoverTileHeight = 260;
  v.homeRecentBooksCount = 3;
  return v;
}();
}  // namespace Lyra3CoversMetrics

class Lyra3CoversTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  int homeCoverThumbHeight(const GfxRenderer& renderer) const override;
};
