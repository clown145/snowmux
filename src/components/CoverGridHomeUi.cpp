#include "CoverGridHomeUi.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "MappedInputManager.h"
#include "UITheme.h"
#include "icons/apps.h"
#include "icons/blocks.h"
#include "icons/book.h"
#include "icons/folder.h"
#include "icons/library.h"
#include "icons/settings2.h"
#include "icons/transfer.h"
#include "util/BookCoverLoader.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId SELECT = 1;
// Grid cell padding around each cover; also feeds the screen's horizontal
// inset so the cover columns land on the header chrome's inset line.
constexpr int16_t COVER_CELL_INSET = 6;
#if FREEINK_DEVICE_XUESU_300
constexpr int16_t COMPACT_GAP = 6;
constexpr int16_t COMPACT_TAB_HEIGHT = 44;
constexpr int SECONDARY_COVERS_PER_PAGE = 2;
constexpr int MAX_DRAWN_COVER_HEIGHT = 128;

const char* compactBookTitle(const RecentBook& book) {
  if (!book.title.empty()) return book.title.c_str();
  const auto lastSlash = book.path.find_last_of('/');
  return lastSlash == std::string::npos ? book.path.c_str() : book.path.c_str() + lastSlash + 1;
}

std::string cachedCompactCover(const std::string& pathTemplate) {
  using State = BookCoverLoader::CachedCoverState;
  if (pathTemplate.empty()) return {};
  const auto preferred = UITheme::getCoverThumbPath(pathTemplate, CoverGridHomeUi::COMPACT_THUMB_HEIGHT);
  if (BookCoverLoader::checkCachedCover(preferred, true) == State::Ready) return preferred;
  if (pathTemplate.find("[HEIGHT]") == std::string::npos) return {};
  const auto separator = pathTemplate.find_last_of('/');
  if (separator == std::string::npos) return {};

  // Reuse thumbnails from other themes and older Cover Grid sizes. Only the
  // per-book cache directory is inspected, and each visible path is resolved once.
  const auto directoryPath = pathTemplate.substr(0, separator);
  int nearestHeight = 0;
  int nearestDistance = 1000;
  if (auto directory = Storage.open(directoryPath.c_str()); directory && directory.isDirectory()) {
    for (int i = 0; i < 32; ++i) {
      auto entry = directory.openNextFile();
      if (!entry) break;
      if (entry.isDirectory() || entry.fileSize() == 0) continue;
      char name[24]{};
      entry.getName(name, sizeof(name));
      const std::string_view fileName(name);
      if (!fileName.starts_with("thumb_") || !fileName.ends_with(".bmp")) continue;
      const auto heightText = fileName.substr(6, fileName.size() - 10);
      int height = 0;
      const auto end = heightText.data() + heightText.size();
      const auto result = std::from_chars(heightText.data(), end, height);
      if (result.ec != std::errc{} || result.ptr != end || height < 32 || height > 512) continue;
      const int distance = std::abs(height - CoverGridHomeUi::COMPACT_THUMB_HEIGHT);
      if (distance >= nearestDistance) continue;
      Bitmap bitmap(entry);
      if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) continue;
      const uint64_t pixelBytes = static_cast<uint64_t>(bitmap.getRowBytes()) * bitmap.getHeight();
      if (bitmap.getPixelDataOffset() + pixelBytes > entry.fileSize()) continue;
      nearestHeight = height;
      nearestDistance = distance;
    }
  }
  if (nearestHeight > 0) return UITheme::getCoverThumbPath(pathTemplate, nearestHeight);
  for (const char* name : {"/cover_legacy_v2.bmp", "/cover.bmp"}) {
    auto full = directoryPath + name;
    if (BookCoverLoader::checkCachedCover(full, true) == State::Ready) return full;
  }
  return {};
}
#else
constexpr int COVER_ROW_EXTRA_HEIGHT = 8;
#endif
}  // namespace

CoverGridHomeUi::CoverGridHomeUi(GfxRenderer& renderer)
    : UiAppHost(renderer), coverCache(renderer), renderer(renderer) {}

void CoverGridHomeUi::begin(const std::vector<RecentBook>& recent, bool opds, bool continuing) {
  books = &recent;
  hasOpds = opds;
  hasContinueReading = continuing;
  if (!recent.empty()) coverCache.begin();
  resetUi();
  app.on(SELECT, &CoverGridHomeUi::onAction, this);
  app.setScreen(&CoverGridHomeUi::screenFn, this);
#if !FREEINK_DEVICE_XUESU_300
  refreshCoverPaths();
#endif
  progress = hasContinueReading && !books->empty() ? loadBookProgress(books->front().path) : -1;
  if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void CoverGridHomeUi::refreshCoverPaths() {
  coverCache.invalidate();
  for (size_t i = 0; i < books->size() && i < coverPaths.size(); ++i) refreshCoverPath(i);
}

void CoverGridHomeUi::refreshCoverPath(size_t index) {
  if (index >= books->size() || index >= coverPaths.size()) return;
  coverCache.invalidate(index);
  coverPaths[index] =
      thumbHeight > 0 ? UITheme::getCoverThumbPath((*books)[index].coverBmpPath, thumbHeight) : std::string();
}

int CoverGridHomeUi::thumbHeightFor() const {
#if FREEINK_DEVICE_XUESU_300
  return COMPACT_THUMB_HEIGHT;
#else
  return thumbHeight > 0 ? thumbHeight : THUMB_HEIGHT;
#endif
}

bool CoverGridHomeUi::takeThumbHeightChanged() { return std::exchange(thumbHeightChanged, false); }

void CoverGridHomeUi::noteThumbHeight(int slotHeight) {
  // One shared height for every slot (hero and grid covers are the same
  // size). The +8 surplus gives the paint clip a little bleed so the art's
  // rightward nudge never exposes the left edge; covers narrower than the
  // 2:3 slot may show a thin sliver at the sides rather than being cropped.
  const int height = std::max(1, slotHeight + 8);
  if (thumbHeight != height) {
    thumbHeight = height;
    thumbHeightChanged = true;
    refreshCoverPaths();
  }
}

void CoverGridHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<CoverGridHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int CoverGridHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  const auto touch = routeTouch(input);
  return touch.snap.touchReleased ? pending : -1;
}

void CoverGridHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<CoverGridHomeUi*>(user)->draw(screen); }

void CoverGridHomeUi::draw(UiScreen& screen) {
  coverCache.prepare();
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  // Sides clear the rounded corners like the header, so the grid lines up under it.
  const int corner = BoardConfig::ACTIVE.cornerRadius;
  const auto left = static_cast<int16_t>(std::max(safe.x, corner));
  const auto right = static_cast<int16_t>(std::max(renderer.getScreenWidth() - safe.x - safe.width, corner));
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(safe.y), right,
                                                static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height),
                                                left});
  // Horizontal inset sized so the outer cover columns (content x plus the
  // grid's cell inset) sit on the theme's side-padding line: the heading and
  // tabs then align with the clock and battery, which tuck a few px further
  // in (headerStatusInset's optical bias).
  const int16_t hInset =
      std::max<int16_t>(0, static_cast<int16_t>(metrics.headerSidePadding - COVER_CELL_INSET - left));
  // Drop the status band, hero, and grid down: touch boards get a full step
  // (no button-hint band to fit), button boards a modest nudge. The tabs stay
  // put (bottom-anchored below).
  const int16_t topInset =
      BoardConfig::hasTouch() ? static_cast<int16_t>(theme.spaceLg) : static_cast<int16_t>(theme.spaceSm + 4);
  screen.insetContent(fui::Insets{topInset, hInset, theme.spaceSm, hInset});
  // Reserve the band's slot in the flow (its content draws at a fixed screen
  // position in drawHeaderBand); the slot doubles as padding above the heading.
  screen.takeTop(metrics.batteryBarHeight, theme.spaceSm);
  // Button boards run tighter above the tabs to make room for the top step.
#if FREEINK_DEVICE_XUESU_300
  auto tabRect = screen.takeBottom(COMPACT_TAB_HEIGHT + screen.target().lineHeight(theme.smallText.font) + COMPACT_GAP,
                                   COMPACT_GAP);
#else
  const int16_t tabGap = BoardConfig::hasTouch() ? theme.spaceSm : 4;
  auto tabRect = screen.takeBottom(metrics.coverGridTabBarHeight, tabGap);
#endif
  const auto& board = BoardConfig::ACTIVE;
  // The board's tab height sits at the bottom of the theme's slot, then its
  // offset lower; the slot itself is unchanged, so the grid keeps its size.
  const auto tabHeight = static_cast<int16_t>(tabRect.height * board.tabBarHeightPct / 100);
  tabRect.y = static_cast<int16_t>(tabRect.y + tabRect.height - tabHeight + board.tabBarOffsetY);
  tabRect.height = tabHeight;
  // The board's share of the row the tabs spread across, centered.
  const auto fitTabWidth = [&board](fui::Rect rect) {
    const auto width = static_cast<int16_t>(rect.width * board.tabBarWidthPct / 100);
    rect.x = static_cast<int16_t>(rect.x + (rect.width - width) / 2);
    rect.width = width;
    return rect;
  };
  if (books->empty()) {
    drawTabs(screen, fitTabWidth(tabRect.inset(fui::Insets{0, COVER_CELL_INSET, 0, COVER_CELL_INSET})));
    drawEmpty(screen);
    drawHeaderBand();
    return;
  }
#if FREEINK_DEVICE_XUESU_300
  drawCompactBooks(screen);
#else
  // Hero cover matches the grid covers: the body splits into three equal
  // cover rows (hero + two grid rows), so the featured section is one row
  // tall and every cover on screen shares one size. The metadata floor still
  // applies when the rows would be too short for the hero's text lines.
  const fui::Rect body = screen.body();
  const int rowGap = std::max(4, body.width / 100);
  grid.gap = grid.rowGap = rowGap;
  const int coverRowHeight = std::max(1, (body.height - theme.spaceSm - 2 * rowGap) / 3 + COVER_ROW_EXTRA_HEIGHT);
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  const int16_t featuredHeight = std::min<int>(
      body.height, std::max<int>(coverRowHeight, screen.target().lineHeight(theme.bodyText.font) * (landscape ? 1 : 2) +
                                                     screen.target().lineHeight(theme.smallText.font) * 2 + 32));
  drawCurrent(screen, screen.takeTop(featuredHeight, theme.spaceSm), coverRowHeight);
#endif
  drawGrid(screen);
#if FREEINK_DEVICE_XUESU_300
  tabRect = tabRect.inset({0, COVER_CELL_INSET, 0, COVER_CELL_INSET});
#else
  tabRect.x = gridBounds.x + grid.cellInset.left;
  tabRect.width = gridBounds.width - grid.cellInset.left - grid.cellInset.right;
#endif
  drawTabs(screen, fitTabWidth(tabRect));
  drawHeaderBand();
}

#if FREEINK_DEVICE_XUESU_300
size_t CoverGridHomeUi::takeMissingVisibleCover() {
  const size_t count = books->size();
  if (count == 0) return MAX_BOOKS;
  const auto tryCover = [&](size_t index) {
    if (coverAttempted[index] || !coverPathChecked[index] || !coverPaths[index].empty()) return false;
    coverAttempted[index] = true;
    return true;
  };
  if (tryCover(0)) return 0;
  const size_t start = 1 + grid.topIndex;
  for (size_t index = start; index < std::min(count, start + SECONDARY_COVERS_PER_PAGE); ++index) {
    if (tryCover(index)) return index;
  }
  return MAX_BOOKS;
}

void CoverGridHomeUi::setLoadedCover(size_t index, std::string path) {
  coverPaths[index] = std::move(path);
  coverCache.invalidate(index);
}

void CoverGridHomeUi::drawCompactBooks(UiScreen& screen) {
  const auto body = screen.body();
  const auto& theme = screen.theme();
  const int secondaryBooks = static_cast<int>(books->size()) - 1;
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  const bool splitColumns = landscape && secondaryBooks > 0;
  grid.columns = SECONDARY_COVERS_PER_PAGE;
  grid.gap = grid.rowGap = COMPACT_GAP;
  if (selected == 0) {
    grid.topIndex = 0;
  } else if (selected < static_cast<int>(books->size())) {
    grid.topIndex = (selected - 1) / SECONDARY_COVERS_PER_PAGE * SECONDARY_COVERS_PER_PAGE;
  }

  const int titleLine = screen.target().lineHeight(theme.bodyText.font);
  const int smallLine = screen.target().lineHeight(theme.smallText.font);
  auto hero = body;
  auto secondary = body;
  if (splitColumns) {
    hero.width = body.width * 2 / 5;
    secondary.x = hero.right() + COMPACT_GAP;
    secondary.width = body.right() - secondary.x;
  } else if (secondaryBooks > 0) {
    hero = screen.takeTop((body.height - COMPACT_GAP) / 2, COMPACT_GAP);
    secondary = screen.body();
  }
  const int metadataHeight = splitColumns ? titleLine + smallLine + 2 * COMPACT_GAP : 0;
  const int maxCoverWidth =
      secondaryBooks > 0 ? (secondary.width - COMPACT_GAP) / grid.columns - 2 * COVER_CELL_INSET : hero.width / 3;
  const int coverHeight =
      std::max(1, std::min({hero.height - 2 * COVER_CELL_INSET - metadataHeight,
                            secondary.height - 2 * COVER_CELL_INSET, maxCoverWidth * 3 / 2, MAX_DRAWN_COVER_HEIGHT}));
  card.coverSize = {static_cast<int16_t>(std::max(1, coverHeight * 2 / 3)), static_cast<int16_t>(coverHeight)};
  gridBounds = layoutGrid(secondary, secondaryBooks > 0 ? 1 : 0);
  if (!splitColumns) gridBounds.y += (secondary.height - gridBounds.height) / 2;
  drawCompactCurrent(screen, hero, splitColumns);
}

void CoverGridHomeUi::drawCompactCurrent(UiScreen& screen, fui::Rect rect, const bool stackMetadata) {
  const auto& book = books->front();
  const auto& theme = screen.theme();
  auto& target = screen.target();
  screen.frame().hit(rect, SELECT, 0, fui::InputTouch);
  const auto content = rect.inset({COMPACT_GAP, COMPACT_GAP, COMPACT_GAP, COMPACT_GAP});
  const int16_t coverX = stackMetadata ? content.x + (content.width - card.coverSize.width) / 2 : content.x;
  const int16_t coverY = stackMetadata ? content.y : content.y + (content.height - card.coverSize.height) / 2;
  const fui::Rect cover{coverX, coverY, card.coverSize.width, card.coverSize.height};
  if (!paintFramedCover(target, cover, 0)) {
    GUI.drawCoverPlaceholder(renderer, Rect{cover.x, cover.y, cover.width, cover.height});
  }
  if (selected == 0) {
    target.fill({rect.x, cover.y, 3, cover.height}, fui::Paint::solid(fui::Color::Black));
  }

  const int16_t textX = stackMetadata ? content.x : cover.right() + 2 * COMPACT_GAP;
  const int16_t textTop = stackMetadata ? cover.bottom() + COMPACT_GAP : content.y;
  const int16_t textAvailableHeight = content.bottom() - textTop;
  const int16_t textWidth = content.right() - textX;
  const int16_t smallLine = target.lineHeight(theme.smallText.font);
  const int16_t progressSpace = progress >= 0 ? smallLine + COMPACT_GAP : 0;
  const int16_t titleLine = target.lineHeight(theme.bodyText.font);
  const int maxTitleLines = stackMetadata ? 1 : 2;
  const bool showAuthor = !book.author.empty() &&
                          textAvailableHeight >= maxTitleLines * titleLine + smallLine + COMPACT_GAP + progressSpace;
  const int authorSpace = showAuthor ? smallLine + COMPACT_GAP : 0;
  auto title = theme.bodyText;
  title.bold = true;
  title.maxLines = std::clamp((textAvailableHeight - authorSpace - progressSpace) / titleLine, 1, maxTitleLines);
  const char* name = compactBookTitle(book);
  const int16_t titleHeight = fui::measureWrappedText(target, name, title, textWidth).height;
  int16_t textY = textTop + (textAvailableHeight - titleHeight - authorSpace - progressSpace) / 2;
  target.text({textX, textY, textWidth, titleHeight}, name, title);
  textY += titleHeight;

  auto detail = theme.smallText;
  detail.maxLines = 1;
  if (showAuthor) {
    target.text({textX, static_cast<int16_t>(textY + COMPACT_GAP), textWidth, smallLine}, book.author.c_str(), detail);
    textY += authorSpace;
  }
  if (progressSpace > 0) {
    textY += COMPACT_GAP;
    const int16_t labelWidth = target.measureText(detail.font, progressText, detail).width;
    target.text({textX, textY, labelWidth, smallLine}, progressText, detail);
    constexpr int16_t barHeight = 5;
    const fui::Rect bar{static_cast<int16_t>(textX + labelWidth + COMPACT_GAP),
                        static_cast<int16_t>(textY + (smallLine - barHeight) / 2),
                        static_cast<int16_t>(textWidth - labelWidth - COMPACT_GAP), barHeight};
    if (bar.width > 0) {
      fui::progressBar(screen.frame(), bar, {.value = progress, .track = fui::Paint::dither(fui::Color::LightGray)});
    }
  }
}
#endif

void CoverGridHomeUi::drawHeaderBand() {
  // The stock full-width band at the theme's topPadding, exactly like every
  // pushed screen's header: the battery/clock hold one position across the
  // whole UI, and the grid is widened to meet them (see the hInset above).
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.batteryBarHeight}, nullptr);
}

void CoverGridHomeUi::drawEmpty(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto body = screen.body();
  auto title = theme.titleText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  auto message = theme.bodyText;
  message.align = fui::TextAlign::Center;
  constexpr int16_t ICON_SIZE = 32;
  const int16_t titleHeight = screen.target().lineHeight(title.font);
  const int16_t messageHeight = screen.target().lineHeight(message.font);
  const int16_t contentHeight = ICON_SIZE + theme.spaceLg + titleHeight + theme.spaceSm + messageHeight;
  int16_t y = body.y + std::max(0, (body.height - contentHeight) / 2);
  renderer.drawIcon(BookIcon, body.x + (body.width - ICON_SIZE) / 2, y, ICON_SIZE);
  y += ICON_SIZE + theme.spaceLg;
  screen.target().text(fui::Rect{body.x, y, body.width, titleHeight}, tr(STR_NO_OPEN_BOOK), title);
  y += titleHeight + theme.spaceSm;
  screen.target().text(fui::Rect{body.x, y, body.width, messageHeight}, tr(STR_START_READING), message);
}

#if !FREEINK_DEVICE_XUESU_300
void CoverGridHomeUi::drawCurrent(UiScreen& screen, fui::Rect rect, const int coverRowHeight) {
  const auto& theme = screen.theme();
  const auto& book = books->front();
  card.title = book.title.c_str();
  card.author = book.author.empty() ? nullptr : book.author.c_str();
  card.meta = nullptr;
  card.progressLabel = progress >= 0 ? progressText : nullptr;
  card.centerTextOnCover = true;
  card.progress = std::max(0, progress);
  card.progressMax = progress >= 0 ? 100 : 0;
  card.action = SELECT;
  // The featured card's selected state is a slim accent bar drawn after the
  // card (see below), not a bookCard indicator: every ring/outline treatment
  // tried here either overwhelmed the large cover or made the heading above
  // read as misaligned.
  card.state = fui::StateNormal;
  card.styles = theme.listRow;
  card.styles.selected.background = fui::Paint::dither(fui::Color::LightGray);
  // The grid thumbs' selection ring draws with this border: gray like Lyra's
  // selection box, not solid black.
  card.styles.selected.border = fui::Paint::dither(fui::Color::LightGray);
  card.styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
  card.styles.selected.radius = theme.listRowRadius;
  card.styles.active = card.styles.selected;
  card.titleText = theme.bodyText;
  card.titleText.maxLines = renderer.getScreenWidth() > renderer.getScreenHeight() ? 1 : 2;
  card.authorText = theme.smallText;
  card.progressText = theme.smallText;
  card.progressHeight = 6;
  card.padding = fui::Insets{6, 6, 6, 6};
  card.gap = theme.spaceLg + theme.spaceSm;
  // Fit the shared 2:3 cover box within both the row and a grid column. No
  // gap reserve: the cell insets already keep adjacent covers apart, and the
  // SpaceBetween layout absorbs whatever slack remains.
  const int maxCoverWidth = rect.width / GRID_COLUMNS - 2 * COVER_CELL_INSET;
  card.coverSize.height = std::max(1, std::min(std::min<int>(rect.height, coverRowHeight) - 12, maxCoverWidth * 3 / 2));
  card.coverSize.width = std::max(1, card.coverSize.height * 2 / 3);
  noteThumbHeight(card.coverSize.height);
  gridBounds = layoutGrid(screen.body(), GRID_ROWS);
  rect.x = gridBounds.x;
  rect.width = gridBounds.width;
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::BookCardProps&, void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, 0);
  };
  fui::bookCard(screen.frame(), rect, card);

  if (selected == 0 && !BoardConfig::hasTouch()) {
    // Button boards only: a vertical accent bar left of the card, cover-height
    // and vertically centered on it, marks the featured card as the button
    // cursor without framing the cover. Touch boards tap directly and need no
    // cursor on the hero card.
    const int16_t barH = card.coverSize.height;
    screen.target().fill(
        fui::Rect{static_cast<int16_t>(rect.x - 5), static_cast<int16_t>(rect.y + (rect.height - barH) / 2), 3, barH},
        fui::Paint::dither(fui::Color::LightGray));
  }
}
#endif

fui::Rect CoverGridHomeUi::layoutGrid(fui::Rect rect, const int rows) {
  // Exactly the hero's box: the three-equal-rows split in draw() already
  // guarantees it fits, and sharing the size keeps one cached thumb per book.
  grid.cellInset = fui::Insets{COVER_CELL_INSET, COVER_CELL_INSET, COVER_CELL_INSET, COVER_CELL_INSET};
  grid.coverSize = card.coverSize;
  grid.rowHeight = grid.coverSize.height + 2 * COVER_CELL_INSET;
  // Full content width: the SpaceBetween column layout pins the outer covers
  // to the rect edges, so the grid reaches the chrome's inset line instead of
  // centering at its natural width.
  rect.height = rows > 0 ? rows * grid.rowHeight + (rows - 1) * grid.rowGap : 0;
  return rect;
}

void CoverGridHomeUi::drawGrid(UiScreen& screen) {
  const auto rect = gridBounds;
  grid.count = books->size() > 1 ? books->size() - 1 : 0;
#if FREEINK_DEVICE_XUESU_300
  grid.minTouchSize = 0;
#else
  grid.columns = GRID_COLUMNS;
#endif
  grid.columnLayout = fui::CoverGridColumnLayout::SpaceBetween;
  grid.action = SELECT;
  grid.inputMask = fui::InputTouch;
  grid.selectedIndex = selected > 0 && selected < static_cast<int>(books->size()) ? selected - 1 : -1;
  // Same thick cover ring as the featured card; the dithered Cell background
  // was easy to miss behind a dark cover.
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  // Thick dithered ring sized for the small thumbs: 6px outside the cover,
  // 2px over its edge.
#if FREEINK_DEVICE_XUESU_300
  grid.selectedCoverFrameGap = 4;
  grid.selectedCoverFrameWidth = 2;
  grid.cellStyles = screen.theme().listRow;
  grid.cellStyles.selected.border = fui::Paint::solid(fui::Color::Black);
#else
  grid.selectedCoverFrameGap = 6;
  grid.selectedCoverFrameWidth = 8;
  grid.cellStyles = card.styles;
#endif
  grid.labelHeight = 0;
  grid.labelGap = 0;
  grid.scrollIndicator = false;
  grid.itemProvider = [](uint16_t index, void*) { return fui::coverGridItem(nullptr, index + 1); };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::CoverGridItem&, uint16_t index,
                         void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, index + 1);
  };
  fui::coverGrid(screen.frame(), rect, grid);
}

void CoverGridHomeUi::drawTabs(UiScreen& screen, fui::Rect rect) {
  static constexpr const uint8_t* ICONS[] = {FolderIcon,   LibraryIcon,   BlocksIcon,
                                             TransferIcon, Settings2Icon, AppsIcon};
  int count = 0;
#if FREEINK_DEVICE_XUESU_300
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  const bool showBookCaption = selected > 0 || landscape;
  const char* caption =
      showBookCaption && selected < static_cast<int>(books->size()) ? compactBookTitle((*books)[selected]) : nullptr;
  const StrId labels[] = {StrId::STR_BROWSE_FILES,
                          StrId::STR_LIBRARY,
                          SETTINGS.pluginsEnabled ? StrId::STR_PLUGINS : StrId::STR_OPDS_BROWSER,
                          StrId::STR_FILE_TRANSFER,
                          StrId::STR_SETTINGS_TITLE,
                          StrId::STR_APPS_TITLE};
#endif
  for (int i = 0; i < 6; ++i) {
    if (i == 2 && !hasOpds) continue;
    auto& tab = tabItems[count];
    tab.value = books->size() + count;
    tab.selected = selected == tab.value;
    tab.label = nullptr;
#if FREEINK_DEVICE_XUESU_300
    if (tab.selected) caption = I18N.get(labels[i]);
#endif
    ++count;
  }
  tabs.tabs = tabItems.data();
  tabs.count = count;
  tabs.layout = fui::TabBarLayout::SpaceBetween;
  tabs.action = SELECT;
  tabs.inputMask = fui::InputTouch;
  tabs.iconSize = 32;
  tabs.iconPainterUserData = this;
  tabs.iconPainter = [](fui::DrawTarget&, fui::Rect iconRect, const fui::TabItem& tab, uint8_t, void* user) {
    const auto& self = *static_cast<CoverGridHomeUi*>(user);
    const int index = tab.value - static_cast<int>(self.books->size());
    const int icon = !self.hasOpds && index >= 2 ? index + 1 : index;
    self.renderer.drawIcon(ICONS[icon], iconRect.x, iconRect.y, iconRect.width);
    return true;
  };
  // No pill fill; the underline marks the selected tab.
  tabs.tabStyles.explicitlySet = true;
  tabs.selectedUnderline = 2;
  tabs.distributedSlotWidth = 0;
#if FREEINK_DEVICE_XUESU_300
  auto captionStyle = screen.theme().smallText;
  captionStyle.maxLines = 1;
  captionStyle.align = fui::TextAlign::Center;
  captionStyle.bold = true;
  const int16_t captionHeight = screen.target().lineHeight(captionStyle.font);
  auto captionRect = fui::Rect{rect.x, rect.y, rect.width, captionHeight};
  const int secondaryCount = std::max(0, static_cast<int>(books->size()) - 1);
  if (secondaryCount > SECONDARY_COVERS_PER_PAGE) {
    char page[24];
    snprintf(page, sizeof(page), "%d/%d", grid.topIndex / SECONDARY_COVERS_PER_PAGE + 1,
             (secondaryCount + SECONDARY_COVERS_PER_PAGE - 1) / SECONDARY_COVERS_PER_PAGE);
    const int16_t pageWidth = screen.target().measureText(captionStyle.font, page, captionStyle).width;
    screen.target().text({static_cast<int16_t>(rect.right() - pageWidth), rect.y, pageWidth, captionHeight}, page,
                         captionStyle);
    captionRect.width -= pageWidth + COMPACT_GAP;
  }
  if (caption) screen.target().text(captionRect, caption, captionStyle);
  rect.y += captionHeight + COMPACT_GAP;
  rect.height -= captionHeight + COMPACT_GAP;
  tabs.tabInset = {0, 3, 4, 3};
  tabs.contentInset = {};
  tabs.gap = COMPACT_GAP;
  tabs.minTouchSize = 0;
#endif
  fui::tabBar(screen.frame(), rect, tabs);
}

bool CoverGridHomeUi::paintFramedCover(fui::DrawTarget& target, fui::Rect rect, size_t index) {
  constexpr int16_t SHADOW_OFFSET = 2;
  const auto ink = fui::Paint::solid(fui::Color::Black);
  target.fill(fui::Rect{rect.right(), static_cast<int16_t>(rect.y + SHADOW_OFFSET), SHADOW_OFFSET, rect.height}, ink);
  target.fill(fui::Rect{static_cast<int16_t>(rect.x + SHADOW_OFFSET), rect.bottom(), rect.width, SHADOW_OFFSET}, ink);
  // The false spine draws inside the cover paint (HomeCoverCache), glued to
  // the art's left edge, so it stays aligned whatever each cover's margin is.
  if (index >= coverPaths.size()) return false;
#if FREEINK_DEVICE_XUESU_300
  if (!coverPathChecked[index]) {
    coverPaths[index] = cachedCompactCover((*books)[index].coverBmpPath);
    coverPathChecked[index] = true;
  }
#endif
  return coverCache.paint(rect, index, coverPaths[index]);
}
