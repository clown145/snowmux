"""Exercise the Cover Grid home using its production drawing and SDK layout."""
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


class XuesuCoverGridTest(unittest.TestCase):
    def test_book_and_settings_focus_fit_every_screen(self):
        source = (ROOT / 'src/components/CoverGridHomeUi.cpp').read_text(encoding='utf-8')
        constants = source[source.index('namespace fui'):source.index('CoverGridHomeUi::CoverGridHomeUi')]
        methods = '\n'.join(method(source, signature) for signature in (
            'void CoverGridHomeUi::draw(UiScreen& screen)',
            'size_t CoverGridHomeUi::takeMissingVisibleCover()',
            'void CoverGridHomeUi::setLoadedCover(',
            'void CoverGridHomeUi::drawCompactBooks(',
            'void CoverGridHomeUi::drawCompactCurrent(',
            'void CoverGridHomeUi::drawEmpty(',
            'fui::Rect CoverGridHomeUi::layoutGrid(',
            'void CoverGridHomeUi::drawGrid(',
            'void CoverGridHomeUi::drawTabs(',
            'bool CoverGridHomeUi::paintFramedCover('))
        sdk = ROOT / 'freeink-sdk/libs/ui/FreeInkUI'
        run_cpp(r'''
#include <FreeInkApp.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "components/Rect.h"

namespace fui = freeink::ui;
using UiRect = fui::Rect;
using UiScreen = fui::Screen<24>;
struct RecentBook { std::string title, author, path, coverBmpPath; };
struct CacheEntry { const char* name; bool complete; };
std::vector<CacheEntry> cachedEntries;
int directoryOpens = 0;
struct CacheFile {
  const CacheEntry* entry = nullptr;
  int next = -1;
  bool isDirectory() const { return next >= 0; }
  size_t fileSize() const { return entry ? (entry->complete ? 10000 : 64) : 0; }
  void getName(char* out, size_t length) const { snprintf(out, length, "%s", entry->name); }
  explicit operator bool() const { return isDirectory() || entry; }
  CacheFile openNextFile() {
    return next >= 0 && next < static_cast<int>(cachedEntries.size()) ? CacheFile{&cachedEntries[next++], -1}
                                                                     : CacheFile{};
  }
};
struct {
  CacheFile open(const char* path) const {
    ++directoryOpens;
    return std::string_view(path) == "/cache" ? CacheFile{nullptr, 0} : CacheFile{};
  }
} Storage;
enum class BmpReaderError { Ok };
struct Bitmap {
  explicit Bitmap(const CacheFile&) {}
  BmpReaderError parseHeaders() const { return BmpReaderError::Ok; }
  int getWidth() const { return 72; }
  int getHeight() const { return 110; }
  int getRowBytes() const { return 12; }
  int getPixelDataOffset() const { return 62; }
};
namespace BookCoverLoader {
enum class CachedCoverState { Missing, Ready, Terminal };
bool canonicalExists = false;
CachedCoverState checkCachedCover(const std::string& path, bool) {
  return path == "cached.bmp" || (canonicalExists && path == "/cache/thumb_136.bmp") ? CachedCoverState::Ready
                                                                                   : CachedCoverState::Missing;
}
}
enum class StrId {
  STR_BROWSE_FILES, STR_LIBRARY, STR_OPDS_BROWSER, STR_PLUGINS, STR_FILE_TRANSFER, STR_SETTINGS_TITLE, STR_APPS_TITLE
};
constexpr const char* STR_NO_OPEN_BOOK = "No open book";
constexpr const char* STR_START_READING = "Start reading";
const char* tr(const char* text) { return text; }
const uint8_t FolderIcon[]{0}, LibraryIcon[]{1}, BlocksIcon[]{2}, TransferIcon[]{3}, Settings2Icon[]{4}, AppsIcon[]{5},
    BookIcon[]{6};
struct { bool pluginsEnabled = false; } SETTINGS;
struct { const char* get(StrId key) const { return key == StrId::STR_SETTINGS_TITLE ? "Settings" : "Menu"; } } I18N;

struct Target : fui::DrawTarget {
  int width, height, small, body;
  int strokes = 0, placeholders = 0;
  std::vector<UiRect> icons;
  std::vector<const uint8_t*> iconImages;
  struct Line { std::string text; UiRect rect; };
  std::vector<Line> lines;
  Target(int w, int h, int s, int b) : width(w), height(h), small(s), body(b) {}
  void inside(UiRect r) const {
    assert(r.x >= 0 && r.y >= 0 && r.width >= 0 && r.height >= 0 && r.right() <= width && r.bottom() <= height);
  }
  int16_t lineHeight(fui::FontId font) const override { return font == fui::FONT_SLOT_SMALL ? small : body; }
  fui::Size measureText(fui::FontId font, const char* text, fui::TextStyle) const override {
    int pixels = 0;
    for (auto p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
      if ((*p & 0xc0) != 0x80) pixels += *p < 0x80 ? small / 2 : lineHeight(font) - 2;
    }
    return {static_cast<int16_t>(pixels), lineHeight(font)};
  }
  void fill(UiRect r, fui::Paint, uint8_t, uint8_t) override { inside(r); }
  void stroke(UiRect r, fui::Paint, uint8_t, uint8_t, uint8_t) override { inside(r); ++strokes; }
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override { assert(false); }
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override { assert(false); }
  void bitmap(UiRect, fui::BitmapRef, fui::BitmapMode, fui::Paint, fui::Rotation) override { assert(false); }
  void text(UiRect r, const char* text, fui::TextStyle style) override {
    inside(r);
    fui::layoutText(*this, r, text, style, [&](const char* line, UiRect bounds) {
      inside(bounds);
      assert(bounds.y >= r.y && bounds.bottom() <= r.bottom());
      assert(measureText(style.font, line, style).width <= r.width);
      lines.push_back({line, bounds});
    });
  }
};
struct GfxRenderer {
  Target& target;
  int getScreenWidth() const { return target.width; }
  int getScreenHeight() const { return target.height; }
  void drawIcon(const uint8_t* image, int x, int y, int size) const {
    const UiRect icon{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(size),
                      static_cast<int16_t>(size)};
    target.inside(icon);
    if (image != BookIcon) {
      target.icons.push_back(icon);
      target.iconImages.push_back(image);
    }
  }
};
struct {
  void drawCoverPlaceholder(GfxRenderer& r, Rect rect) {
    r.target.inside({static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y),
                     static_cast<int16_t>(rect.width), static_cast<int16_t>(rect.height)});
    ++r.target.placeholders;
  }
} GUI;
namespace BoardConfig {
inline constexpr struct { int cornerRadius = 0, tabBarHeightPct = 100, tabBarOffsetY = 0, tabBarWidthPct = 100; }
    ACTIVE;
bool hasTouch() { return false; }
}
struct UITheme {
  struct Metrics { int headerSidePadding = 18, batteryBarHeight = 20; } metrics;
  static UITheme& getInstance() { static UITheme theme; return theme; }
  const auto& getMetrics() const { return metrics; }
  Rect getScreenSafeArea(GfxRenderer& r, bool) {
    return Rect{0, 0, r.getScreenWidth(), r.getScreenHeight()};
  }
  static std::string getCoverThumbPath(std::string path, int height) {
    const auto token = path.find("[HEIGHT]");
    if (token != std::string::npos) path.replace(token, 8, std::to_string(height));
    return path;
  }
};
struct CoverGridHomeUi {
  struct CoverCache {
    std::vector<std::pair<size_t, UiRect>> painted;
    void prepare() { painted.clear(); }
    bool paint(UiRect rect, size_t index, const std::string& path) {
      painted.emplace_back(index, rect);
      return !path.empty();
    }
    void invalidate(size_t) {}
  } coverCache;
  static constexpr int GRID_COLUMNS = 3, GRID_ROWS = 2, MAX_BOOKS = 7;
  static constexpr int COMPACT_THUMB_HEIGHT = 136;
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books;
  std::array<std::string, MAX_BOOKS> coverPaths;
  std::array<bool, MAX_BOOKS> coverPathChecked{}, coverAttempted{};
  int thumbHeight = 0, selected = 0, progress = 72;
  bool hasOpds = false;
  const char* progressText = "72%";
  fui::BookCardProps card;
  fui::CoverGridProps grid;
  UiRect gridBounds{};
  fui::TabBarProps tabs;
  std::array<fui::TabItem, 6> tabItems;
  CoverGridHomeUi(GfxRenderer& r, const std::vector<RecentBook>& data) : renderer(r), books(&data) {}
  void draw(UiScreen&);
  void drawCompactBooks(UiScreen&);
  void drawCompactCurrent(UiScreen&, UiRect, bool);
  void drawEmpty(UiScreen&);
  UiRect layoutGrid(UiRect, int);
  void drawGrid(UiScreen&);
  void drawTabs(UiScreen&, UiRect);
  bool paintFramedCover(fui::DrawTarget&, UiRect, size_t);
  size_t takeMissingVisibleCover();
  void setLoadedCover(size_t, std::string);
  void drawHeaderBand() {}
  void noteThumbHeight(int height) { thumbHeight = height; }
};
''' + constants + methods + r'''
int main() {
  const std::string path = "/cache/thumb_[HEIGHT].bmp";
  cachedEntries = {{"thumb_90.bmp", true}, {"thumb_140.bmp", false}, {"thumb_118.bmp", true}};
  assert(cachedCompactCover(path) == "/cache/thumb_118.bmp" && directoryOpens == 1);
  BookCoverLoader::canonicalExists = true;
  assert(cachedCompactCover(path) == "/cache/thumb_136.bmp" && directoryOpens == 1);
  const std::string longTitle = "这一段很长的中文书名用于验证封面网格能完整显示两行文字且不会挡住进度";
  for (const int width : {300, 400}) for (int count = 0; count <= 7; ++count)
  for (const auto& [small, body] : {std::pair{16, 20}, std::pair{24, 30}}) for (const bool optionalTab : {false, true})
  for (const int progress : {-1, 72}) {
    const int height = 700 - width;
    const bool cached = count % 2 == 0;
    const std::vector<RecentBook> books(count, {longTitle, longTitle, "/a/book.txt", cached ? "cached.bmp" : ""});
    for (int focus = 0; focus < count + 5 + optionalTab; ++focus) {
      Target target(width, height, small, body);
      GfxRenderer renderer{target};
      CoverGridHomeUi home(renderer, books);
      home.hasOpds = optionalTab;
      home.selected = focus;
      home.progress = progress;
      fui::DeviceContext device{};
      device.width = width;
      device.height = height;
      fui::InteractionBuffer<24> interactions;
      const fui::InputSnapshot input{};
      fui::Frame<24> frame(target, device, input, interactions);
      const auto theme = fui::themeTokensForLineHeight(body);
      UiScreen screen(frame, theme);
      home.draw(screen);
      const int pageStart = focus > 0 && focus < count ? (focus - 1) / 2 * 2 : 0;
      const int visibleBooks = count > 0 ? 1 + std::min(2, count - 1 - pageStart) : 0;
      const int controlCount = visibleBooks + 5 + optionalTab;
      assert(interactions.count() == static_cast<size_t>(controlCount));
      const auto* controls = interactions.data();
      for (int i = 0; i < controlCount; ++i) {
        const int value = i == 0 && count > 0 ? 0 : i < visibleBooks ? i + pageStart : count + i - visibleBooks;
        assert(controls[i].value == value);
        for (int j = 0; j < i; ++j) {
          const auto& a = controls[i].rect;
          const auto& b = controls[j].rect;
          assert(a.x >= b.right() + 6 || b.x >= a.right() + 6 || a.y >= b.bottom() + 6 || b.y >= a.bottom() + 6);
        }
      }
      const auto& slots = home.coverCache.painted;
      assert(static_cast<int>(slots.size()) == visibleBooks);
      for (int i = 0; i < visibleBooks; ++i) {
        const auto [index, rect] = slots[i];
        assert(static_cast<int>(index) == (i > 0 ? i + pageStart : 0));
        assert(rect.width >= 60 && rect.height >= 90);
        if (i > 0 && width > height) assert(rect.y == slots[0].second.y);
        if (!cached) assert(home.takeMissingVisibleCover() == index);
      }
      assert(home.takeMissingVisibleCover() == static_cast<size_t>(home.MAX_BOOKS));
      assert(target.strokes == (focus > 0 && focus < count ? 1 : 0));
      assert(target.placeholders == count % 2);
      assert(static_cast<int>(target.icons.size()) == 5 + optionalTab);
      for (size_t i = 1; i < target.icons.size(); ++i) {
        assert(target.icons[i].x - target.icons[i-1].right() >= 6);
      }
      const int settings = count + 3 + optionalTab;
      assert(home.tabItems[3 + optionalTab].value == settings);
      assert(target.iconImages[3 + optionalTab] == Settings2Icon);
      const bool settingsDrawn = std::any_of(target.lines.begin(), target.lines.end(),
                                             [](const auto& line) { return line.text == "Settings"; });
      assert(settingsDrawn == (focus == settings));
    }
  }
}
''', include_dirs=[ROOT / 'src', sdk / 'include'], defines=['FREEINK_DEVICE_XUESU_300=1'],
                extra_sources=[sdk / 'src/FreeInkUI.cpp'])

    def test_idle_generation_skips_unchanged_failures_after_reentry(self):
        source = (ROOT / 'src/activities/home/HomeActivity.cpp').read_text(encoding='utf-8')
        helpers = source[source.index('struct GridCoverFailure {'):source.index('uint32_t HomeActivity::idleRenderDelayMs()')]
        helpers = helpers[:helpers.rindex('}  // namespace')]
        idle = method(source, 'void HomeActivity::renderIdle(')
        run_cpp(r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define LOG_ERR(...) ((void)0)
int epubAttempts = 0, xtcAttempts = 0;
uint32_t sourceModified = 1;
struct File {
  explicit operator bool() const { return true; }
  bool isDirectory() const { return false; }
  uint64_t fileSize64() const { return 100; }
  uint32_t modificationTime() const { return sourceModified; }
};
struct {
  File open(const char*) const { return {}; }
  bool exists(const char*) const { return false; }
} Storage;
namespace FsHelpers {
bool hasReflowableBookExtension(const std::string& path) { return path.ends_with(".epub"); }
bool hasXtcExtension(const std::string& path) { return path.ends_with(".xtc"); }
}
namespace BookCoverLoader {
enum class CachedCoverState { Ready, Missing, Terminal };
CachedCoverState checkCachedCover(const std::string&, bool) { return CachedCoverState::Missing; }
std::string ensureThumbnail(const std::string&, int) { ++xtcAttempts; return "xtc-cover.bmp"; }
}
struct Epub {
  Epub(const std::string&, const char*) {}
  std::string getThumbBmpPath(int) const { return "epub-thumb.bmp"; }
  std::string getCoverOverridePath() const { return "override.bmp"; }
  bool generateThumbBmpFromSource(int) { ++epubAttempts; return false; }
};
template <class T, class... Args> std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}
struct GfxRenderer {
  struct FrameBufferLoan { explicit FrameBufferLoan(GfxRenderer&) {} ~FrameBufferLoan() {} };
};
struct {
  bool cancelled = false;
  bool idleRenderCancelled(uint32_t) const { return cancelled; }
} activityManager;
struct CoverGridHomeUi {
  static constexpr size_t MAX_BOOKS = 7;
  std::vector<size_t> missing;
  std::vector<size_t> loaded;
  size_t consumed = 0;
  size_t takeMissingVisibleCover() { return consumed < missing.size() ? missing[consumed++] : MAX_BOOKS; }
  int thumbHeightFor() const { return 136; }
  void setLoadedCover(size_t i, std::string path) { assert(path == "xtc-cover.bmp"); loaded.push_back(i); }
};
struct HomeActivity {
  CoverGridHomeUi* coverGridUi;
  GfxRenderer renderer;
  struct Book { std::string path; };
  std::vector<Book> recentBooks{{"/plain.epub"}, {"/illustrated.xtc"}};
  int repaints = 0;
  explicit HomeActivity(CoverGridHomeUi& ui) : coverGridUi(&ui) {}
  void renderIdle(uint32_t);
  void requestUpdate() { ++repaints; }
};
''' + helpers + idle + r'''
int main() {
  CoverGridHomeUi coldGrid;
  coldGrid.missing = {0, 1};
  HomeActivity cold(coldGrid);
  activityManager.cancelled = true;
  cold.renderIdle(1);
  assert(coldGrid.consumed == 0 && epubAttempts == 0 && xtcAttempts == 0);
  activityManager.cancelled = false;
  cold.renderIdle(1);
  assert(epubAttempts == 1 && xtcAttempts == 0 && cold.repaints == 1);
  cold.renderIdle(1);
  assert(epubAttempts == 1 && xtcAttempts == 1 && coldGrid.loaded == std::vector<size_t>{1});

  // New HomeActivity, same book and successful XTC now resolved by its BMP.
  CoverGridHomeUi warmGrid;
  warmGrid.missing = {0};
  HomeActivity warm(warmGrid);
  warm.renderIdle(2);
  assert(epubAttempts == 1 && xtcAttempts == 1 && warm.repaints == 0);

  ++sourceModified;
  CoverGridHomeUi changedGrid;
  changedGrid.missing = {0};
  HomeActivity changed(changedGrid);
  changed.renderIdle(3);
  assert(epubAttempts == 2 && changed.repaints == 1);
}
''')
