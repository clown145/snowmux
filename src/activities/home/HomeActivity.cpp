#include "HomeActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/CompactHomeLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCoverLoader.h"

namespace {
struct HomeMenuEntry {
  HomeMenuItem item;
  StrId label;
  UIIcon icon;
};

constexpr HomeMenuEntry kDefaultMenuOrder[] = {
    {HomeMenuItem::FILE_BROWSER, StrId::STR_BROWSE_FILES, Folder},
    {HomeMenuItem::LIBRARY, StrId::STR_LIBRARY, Library},
    {HomeMenuItem::OPDS_BROWSER, StrId::STR_OPDS_BROWSER, Blocks},
    {HomeMenuItem::FILE_TRANSFER, StrId::STR_FILE_TRANSFER, Transfer},
    {HomeMenuItem::SETTINGS_MENU, StrId::STR_SETTINGS_TITLE, Settings},
    {HomeMenuItem::APPS, StrId::STR_APPS_TITLE, Apps},
};
constexpr HomeMenuEntry kCarouselMenuOrder[] = {
    {HomeMenuItem::FILE_BROWSER, StrId::STR_BROWSE_FILES, Folder},
    {HomeMenuItem::RECENTS, StrId::STR_MENU_RECENT_BOOKS, Recent},
    {HomeMenuItem::OPDS_BROWSER, StrId::STR_OPDS_BROWSER, Library},
    {HomeMenuItem::APPS, StrId::STR_APPS_TITLE, Apps},
    {HomeMenuItem::FILE_TRANSFER, StrId::STR_FILE_TRANSFER, Transfer},
    {HomeMenuItem::SETTINGS_MENU, StrId::STR_SETTINGS_TITLE, Settings},
};
constexpr int kHomeMenuItemCount = 6;

constexpr const HomeMenuEntry* menuEntryAtIndex(int index, bool hasOpds, bool carousel) {
  if (index < 0) return nullptr;
  if (!hasOpds && index >= 2) ++index;
  if (index >= kHomeMenuItemCount) return nullptr;
  return &(carousel ? kCarouselMenuOrder : kDefaultMenuOrder)[index];
}

constexpr HomeMenuItem indexToMenuItem(int index, bool hasOpds, bool carousel) {
  const HomeMenuEntry* entry = menuEntryAtIndex(index, hasOpds, carousel);
  return entry == nullptr ? HomeMenuItem::NONE : entry->item;
}

constexpr int menuItemToIndex(HomeMenuItem item, bool hasOpds, bool carousel) {
  const int count = hasOpds ? kHomeMenuItemCount : kHomeMenuItemCount - 1;
  for (int i = 0; i < count; ++i) {
    if (indexToMenuItem(i, hasOpds, carousel) == item) return i;
  }
  return 0;
}

static_assert(indexToMenuItem(2, false, true) == HomeMenuItem::APPS);
static_assert(indexToMenuItem(4, false, true) == HomeMenuItem::SETTINGS_MENU);
static_assert(indexToMenuItem(3, true, true) == HomeMenuItem::APPS);
static_assert(indexToMenuItem(5, true, true) == HomeMenuItem::SETTINGS_MENU);
static_assert(indexToMenuItem(2, false, false) == HomeMenuItem::FILE_TRANSFER);
static_assert(indexToMenuItem(4, false, false) == HomeMenuItem::APPS);
static_assert(menuItemToIndex(HomeMenuItem::APPS, false, true) == 2);
static_assert(menuItemToIndex(HomeMenuItem::SETTINGS_MENU, true, true) == 5);
}  // namespace

int HomeActivity::getMenuItemCount() const {
  return static_cast<int>(recentBooks.size()) + kHomeMenuItemCount - !hasLibrarySlot();
}

void HomeActivity::requestCarouselUpdate(CarouselUpdateScope scope) {
  if (scope == CarouselUpdateScope::Full) {
    carouselUpdateScope = scope;
  } else {
    // A queued full redraw must not be downgraded by a later menu event.
    CarouselUpdateScope expected = CarouselUpdateScope::None;
    carouselUpdateScope.compare_exchange_strong(expected, CarouselUpdateScope::MenuOnly);
  }
  requestUpdate();
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
#if FREEINK_DEVICE_XUESU_300
    // Home can use recent books immediately. Index creation belongs to Library.
    return;
#else
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
#endif
  }
  const int candidates =
      FREEINK_DEVICE_XUESU_300 ? std::min<int>(index.bookCount(), CoverGridHomeUi::MAX_BOOKS * 4) : index.bookCount();
  for (uint16_t row = 0; row < candidates && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

#if !FREEINK_DEVICE_XUESU_300
void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  const bool isEpub = FsHelpers::hasReflowableBookExtension(book.path);
  if (!book.coverBmpPath.empty() &&
      BookCoverLoader::checkCachedCover(UITheme::getCoverThumbPath(book.coverBmpPath, height), isEpub) !=
          BookCoverLoader::CachedCoverState::Missing)
    return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (isEpub) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (BookCoverLoader::checkCachedCover(epub->getThumbBmpPath(height), true) !=
        BookCoverLoader::CachedCoverState::Missing)
      return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (BookCoverLoader::checkCachedCover(xtc->getThumbBmpPath(height), false) !=
        BookCoverLoader::CachedCoverState::Missing)
      return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}
#endif

#if FREEINK_DEVICE_XUESU_300
namespace {
struct GridCoverFailure {
  uint64_t size = 0;
  size_t pathHash = 0;
  uint32_t modified = 0;

  bool operator==(const GridCoverFailure&) const = default;
};

// HomeActivity is replaced when a book or another screen opens. Keep only
// compact failure stamps across those visits, never another framebuffer.
std::array<GridCoverFailure, CoverGridHomeUi::MAX_BOOKS * 2> gridCoverFailures{};
size_t nextGridCoverFailure = 0;

GridCoverFailure gridCoverSource(const std::string& path) {
  auto file = Storage.open(path.c_str());
  if (!file || file.isDirectory()) return {};
  return {file.fileSize64(), std::hash<std::string>{}(path), file.modificationTime()};
}

bool skipGridCoverSource(const GridCoverFailure& source) {
  return source.size == 0 ||
         std::find(gridCoverFailures.begin(), gridCoverFailures.end(), source) != gridCoverFailures.end();
}

void rememberGridCoverFailure(const GridCoverFailure& source) {
  gridCoverFailures[nextGridCoverFailure] = source;
  nextGridCoverFailure = (nextGridCoverFailure + 1) % gridCoverFailures.size();
}
}  // namespace

uint32_t HomeActivity::idleRenderDelayMs() const { return coverGridUi ? 450 : 0; }

void HomeActivity::renderIdle(const uint32_t generation) {
  if (!coverGridUi || activityManager.idleRenderCancelled(generation)) return;
  // One conversion per idle pass. A reader or menu transition cancels pending
  // work and the last complete frame stays visible throughout conversion.
  for (size_t index = coverGridUi->takeMissingVisibleCover(); index < recentBooks.size();
       index = coverGridUi->takeMissingVisibleCover()) {
    const auto& book = recentBooks[index];
    const bool reflowable = FsHelpers::hasReflowableBookExtension(book.path);
    if (!reflowable && !FsHelpers::hasXtcExtension(book.path)) continue;
    const auto source = gridCoverSource(book.path);
    if (skipGridCoverSource(source)) continue;
    if (activityManager.idleRenderCancelled(generation)) return;

    const int height = coverGridUi->thumbHeightFor();
    std::string generated;
    if (reflowable) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: compact grid cover");
        return;
      }
      const auto path = epub->getThumbBmpPath(height);
      using State = BookCoverLoader::CachedCoverState;
      const auto cache = BookCoverLoader::checkCachedCover(path, !Storage.exists(epub->getCoverOverridePath().c_str()));
      if (cache == State::Ready) {
        generated = path;
      } else if (cache == State::Missing) {
        const GfxRenderer::FrameBufferLoan loan(renderer);
        if (epub->generateThumbBmpFromSource(height) && BookCoverLoader::checkCachedCover(path, true) == State::Ready) {
          generated = path;
        }
      }
    } else {
      generated = BookCoverLoader::ensureThumbnail(book.path, height);
    }
    if (generated.empty()) {
      rememberGridCoverFailure(source);
    } else {
      coverGridUi->setLoadedCover(index, std::move(generated));
    }
    // Also repaint after a failed EPUB attempt so the loaned framebuffer
    // contains a complete frame again. An unchanged frame is skipped by the panel.
    requestUpdate();
    return;
  }
}
#endif

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;
  const bool useFullCover = !FREEINK_DEVICE_XUESU_300 && UITheme::getInstance().hasCarouselHome();

  int progress = 0;
  for (RecentBook& book : recentBooks) {
#if !FREEINK_DEVICE_XUESU_300
    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
#endif
    const int currentProgress = progress++;
    const bool isEpub = FsHelpers::hasReflowableBookExtension(book.path);
    const bool isXtc = FsHelpers::hasXtcExtension(book.path);

    // Keep the persisted path theme-neutral; Carousel redirects only this activity's copy.
    if (isEpub) {
      const Epub epub(book.path, "/.crosspoint");
      if (useFullCover) {
        const std::string thumbPath = epub.getThumbBmpPath();
        if (book.coverBmpPath != thumbPath) {
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, thumbPath);
        }
        book.coverBmpPath = epub.getCoverBmpPath();
      } else if (book.coverBmpPath.empty()) {
        book.coverBmpPath = epub.getThumbBmpPath();
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
      }
    } else if (isXtc) {
      const Xtc xtc(book.path, "/.crosspoint");
      if (useFullCover) {
        const std::string thumbPath = xtc.getThumbBmpPath();
        if (book.coverBmpPath != thumbPath) {
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, thumbPath);
        }
        book.coverBmpPath = xtc.getCoverBmpPath();
      } else if (book.coverBmpPath.empty()) {
        book.coverBmpPath = xtc.getThumbBmpPath();
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
      }
    } else {
      continue;
    }
    if (book.coverBmpPath.empty()) continue;

    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
    // EPUB thumbnails keep an empty file as the persistent "no supported cover" marker.
    if (BookCoverLoader::checkCachedCover(coverPath, isEpub && !useFullCover) !=
        BookCoverLoader::CachedCoverState::Missing)
      continue;

    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    }
    GUI.fillPopupProgress(renderer, popupRect, 10 + currentProgress * (90 / recentBooks.size()));
    std::string generatedPath;
    if (isEpub) {
      {
        GfxRenderer::FrameBufferLoan loan(renderer);
        generatedPath = useFullCover ? BookCoverLoader::ensureFullCover(book.path)
                                     : BookCoverLoader::ensureThumbnail(book.path, coverHeight);
      }
      // Inflate used the old framebuffer bytes. Rebuild a complete, known
      // loading frame before the next progress refresh can reach the panel.
      renderer.clearScreen();
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 10 + currentProgress * (90 / recentBooks.size()));
    } else {
      generatedPath = useFullCover ? BookCoverLoader::ensureFullCover(book.path)
                                   : BookCoverLoader::ensureThumbnail(book.path, coverHeight);
    }
    if (generatedPath.empty() && isXtc) LOG_ERR("HOME", "Failed to generate XTC cover: %s", book.path.c_str());
  }

  recentsLoaded = true;
  recentsLoading = false;
  coverRendered = false;
  coverBufferStored = false;
  requestCarouselUpdate(CarouselUpdateScope::Full);
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  // Plugin System is the global opt-in. Once enabled, expose the Plugins entry
  // even when no SD plugins are installed yet so Plugin Hub can be installed
  // from inside the Plugins screen (or users can sideload plugins manually).
  hasPlugins = SETTINGS.pluginsEnabled != 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  loadRecentBooks(coverGridUi ? CoverGridHomeUi::MAX_BOOKS : metrics.homeRecentBooksCount);
  hasContinueReading = !recentBooks.empty();
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    coverGridUi->begin(recentBooks, hasLibrarySlot(), hasContinueReading);
  }

  const auto base = static_cast<int>(recentBooks.size());
  const bool isCarousel = UITheme::getInstance().hasCarouselHome();
  selectorIndex =
      initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasLibrarySlot(), isCarousel);
  lastCarouselBookIndex = 0;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  if (coverBufferUnavailable) return false;

  // Thumbnail generation may borrow the framebuffer; cache only the final render.
  if (!recentsLoaded) return false;

  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;

  if (!coverBuffer || coverBufferSize < needed) {
    // The carousel region is up to ~44 KB, too large for the task stack. Allocate
    // once and reuse it for every selection during this HomeActivity lifetime.
    auto replacement = makeUniqueNoThrow<uint8_t[]>(needed);
    if (!replacement) {
      LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
      // ponytail: the theme/region is fixed for this Activity lifetime; retry
      // only after re-entering Home, when heap fragmentation may have changed.
      coverBufferUnavailable = true;
      return false;
    }
    coverBuffer = std::move(replacement);
    coverBufferSize = needed;
  }

  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(),
                                   coverBufferSize)) {
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(),
                                     coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  coverBuffer.reset();
  coverBufferSize = 0;
  coverBufferStored = false;
  coverBufferUnavailable = false;
}

void HomeActivity::loop() {
  // Xuesu has one pair of direction keys. They must reach both books and tabs.
#if FREEINK_DEVICE_XUESU_300
  constexpr bool flatKeyNavigation = true;
#else
  constexpr bool flatKeyNavigation = false;
#endif
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bookCount = static_cast<int>(recentBooks.size());
  const int renderedMenuCount = menuCount - (metrics.homeContinueReadingInMenu ? 0 : bookCount);
  const bool isCarousel = UITheme::getInstance().hasCarouselHome();

  auto activateSelection = [this, isCarousel, bookCount] {
    if (selectorIndex < bookCount) {
      onRecentBookSelected(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - bookCount;
    switch (indexToMenuItem(menuIndex, hasLibrarySlot(), isCarousel)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:  // the library slot
        hasPlugins ? onPluginsOpen() : onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      case HomeMenuItem::APPS:
        onAppsOpen();
        break;
      default:
        break;
    }
  };

  if (isCarousel && !flatKeyNavigation) {
    const bool coversFocused = selectorIndex < bookCount;
    const int rowIndex = coversFocused ? selectorIndex : selectorIndex - bookCount;

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (coversFocused) {
        selectorIndex = ButtonNavigator::nextIndex(rowIndex, bookCount);
        lastCarouselBookIndex = selectorIndex;
      } else {
        selectorIndex = bookCount + ButtonNavigator::nextIndex(rowIndex, renderedMenuCount);
      }
      requestCarouselUpdate(coversFocused ? CarouselUpdateScope::Full : CarouselUpdateScope::MenuOnly);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (coversFocused) {
        selectorIndex = ButtonNavigator::previousIndex(rowIndex, bookCount);
        lastCarouselBookIndex = selectorIndex;
      } else {
        selectorIndex = bookCount + ButtonNavigator::previousIndex(rowIndex, renderedMenuCount);
      }
      requestCarouselUpdate(coversFocused ? CarouselUpdateScope::Full : CarouselUpdateScope::MenuOnly);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (bookCount > 0) {
        if (coversFocused) {
          lastCarouselBookIndex = selectorIndex;
          selectorIndex = bookCount;
        } else {
          selectorIndex = std::clamp(lastCarouselBookIndex, 0, bookCount - 1);
        }
        requestCarouselUpdate(CarouselUpdateScope::Full);
      }
      return;
    }
  } else if (!coverGridUi || flatKeyNavigation) {
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  // The standby shortcut keeps ownership of Back when enabled.
  if (!flatKeyNavigation && !SETTINGS.standbyShortcutEnabled &&
      mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    onRecentBookSelected(recentBooks[0].path);
    return;
  }

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    if (flatKeyNavigation) return;
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int coverCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      selectorIndex = base + idx;
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, coverCount] { cycleBand(0, coverCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, coverCount] { cycleBand(0, coverCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [&cycleBand, coverCount, menuCount] {
      cycleBand(coverCount, menuCount - coverCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [&cycleBand, coverCount, menuCount] {
      cycleBand(coverCount, menuCount - coverCount, +1);
    });
    return;
  }

  if (flatKeyNavigation) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) activateSelection();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (isCarousel) {
    const bool coversFocused = selectorIndex < bookCount;
    const int rowIndex = coversFocused ? selectorIndex : selectorIndex - bookCount;
    switch (swipe) {
      case MappedInputManager::SwipeDir::Left:
        if (coversFocused) {
          selectorIndex = ButtonNavigator::nextIndex(rowIndex, bookCount);
          lastCarouselBookIndex = selectorIndex;
        } else {
          selectorIndex = bookCount + ButtonNavigator::nextIndex(rowIndex, renderedMenuCount);
        }
        requestCarouselUpdate(coversFocused ? CarouselUpdateScope::Full : CarouselUpdateScope::MenuOnly);
        return;
      case MappedInputManager::SwipeDir::Right:
        if (coversFocused) {
          selectorIndex = ButtonNavigator::previousIndex(rowIndex, bookCount);
          lastCarouselBookIndex = selectorIndex;
        } else {
          selectorIndex = bookCount + ButtonNavigator::previousIndex(rowIndex, renderedMenuCount);
        }
        requestCarouselUpdate(coversFocused ? CarouselUpdateScope::Full : CarouselUpdateScope::MenuOnly);
        return;
      case MappedInputManager::SwipeDir::Up:
      case MappedInputManager::SwipeDir::Down:
        if (bookCount > 0) {
          if (coversFocused) {
            lastCarouselBookIndex = selectorIndex;
            selectorIndex = bookCount;
          } else {
            selectorIndex = std::clamp(lastCarouselBookIndex, 0, bookCount - 1);
          }
          requestCarouselUpdate(CarouselUpdateScope::Full);
        }
        return;
      case MappedInputManager::SwipeDir::None:
        break;
    }
  } else {
    if (swipe == MappedInputManager::SwipeDir::Up) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
      return;
    }
  }

  const Rect content = isCarousel ? Rect{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()}
                                  : UITheme::getContentArea(renderer);
  int tx = 0;
  int ty = 0;
  if (!recentBooks.empty() && mappedInput.wasScreenTouchDown(tx, ty) && tx >= content.x &&
      tx < content.x + content.width && ty >= metrics.homeTopPadding &&
      ty < metrics.homeTopPadding + metrics.homeCoverTileHeight) {
    int touchedBook = 0;
    if (isCarousel) {
      const int centerBook =
          selectorIndex < bookCount ? selectorIndex : std::clamp(lastCarouselBookIndex, 0, bookCount - 1);
      if (tx < renderer.getScreenWidth() / 3) {
        touchedBook = ButtonNavigator::previousIndex(centerBook, bookCount);
      } else if (tx >= renderer.getScreenWidth() * 2 / 3) {
        touchedBook = ButtonNavigator::nextIndex(centerBook, bookCount);
      } else {
        touchedBook = centerBook;
      }
      lastCarouselBookIndex = touchedBook;
    } else {
      // Multi-cover themes (Lyra3Covers and the like) render several recent
      // books side by side: map the finger to the cover it is on instead of
      // always settling on the first book.
      touchedBook = GUI.recentBookIndexAt(tx - content.x, content.width);
      touchedBook = std::clamp(touchedBook, 0, static_cast<int>(recentBooks.size()) - 1);
    }
    if (selectorIndex != touchedBook) {
      selectorIndex = touchedBook;
      requestCarouselUpdate(CarouselUpdateScope::Full);
    }
    return;
  }

  int tapX = 0;
  int tapY = 0;
  if (!recentBooks.empty() && mappedInput.wasScreenTapped(tapX, tapY) && tapX >= content.x &&
      tapX < content.x + content.width && tapY >= metrics.homeTopPadding &&
      tapY < metrics.homeTopPadding + metrics.homeCoverTileHeight) {
    if (!isCarousel) {
      selectorIndex = GUI.recentBookIndexAt(tapX - content.x, content.width);
      selectorIndex = std::clamp(selectorIndex, 0, static_cast<int>(recentBooks.size()) - 1);
    }
    haptic_feedback::touchAction();
    activateSelection();
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  int menuRow = -1;
  MappedInputManager::RowTouch menuTouch;
  BaseTheme::MenuRowGeometry menuGeo{};
  if (isCarousel) {
    const int menuBottom = renderer.getScreenHeight();
    const int columnWidth = renderer.getScreenWidth() / renderedMenuCount;
    menuTouch = mappedInput.colTouch(menuRow, 0, columnWidth, renderedMenuCount, menuBottom - metrics.menuRowHeight,
                                     menuBottom, columnWidth);
  } else {
    // Hit-test from the theme's drawn menu geometry, never from a fixed metrics
    // table: RoundedRaff/Inx derive the row height from the font and page the
    // rows, Lyra draws rows without the Base vertical offset (see BaseTheme.h).
    const Rect menuRect{
        content.x, menuTop, content.width,
        renderer.getScreenHeight() - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                                      metrics.homeMenuTopOffset + metrics.buttonHintsHeight)};
    menuGeo = GUI.getMenuRowGeometry(renderer, menuRect, renderedMenuSelection, renderedMenuCount);
    menuTouch = mappedInput.rowTouch(menuRow, menuGeo.firstRowY, menuGeo.rowStep, menuGeo.pageCount, menuGeo.xStart,
                                     menuGeo.xEnd, menuGeo.rowHeight);
  }
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        menuGeo.pageStart +
        (metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size()));
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        if (isCarousel) {
          requestCarouselUpdate(CarouselUpdateScope::MenuOnly);
        } else {
          requestUpdate();
        }
      }
    } else {
      selectorIndex = touchedIndex;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  static_assert(canRenderCarouselMenuOnly(true, true, CarouselUpdateScope::MenuOnly));
  static_assert(!canRenderCarouselMenuOnly(false, true, CarouselUpdateScope::MenuOnly));
  static_assert(!canRenderCarouselMenuOnly(true, false, CarouselUpdateScope::MenuOnly));
  static_assert(!canRenderCarouselMenuOnly(true, true, CarouselUpdateScope::Full));

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool isCarousel = UITheme::getInstance().hasCarouselHome();

  const char* backLabel = hasContinueReading ? tr(STR_RESUME) : "";
  if (SETTINGS.standbyShortcutEnabled) backLabel = tr(STR_STANDBY_TITLE);

  const Rect content = isCarousel ? Rect{0, 0, pageWidth, pageHeight} : UITheme::getContentArea(renderer);
  const int homeMenuItemCount = hasLibrarySlot() ? kHomeMenuItemCount : kHomeMenuItemCount - 1;
  const bool showContinueReading = metrics.homeContinueReadingInMenu && !recentBooks.empty();
  const int menuItemCount = homeMenuItemCount + showContinueReading;

  Rect headerRect{0, metrics.topPadding, pageWidth,
                  isCarousel ? metrics.homeTopPadding : metrics.homeTopPadding - metrics.topPadding};
  Rect coverRect{content.x, metrics.homeTopPadding, content.width, metrics.homeCoverTileHeight};
  Rect menuRect{content.x, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset,
                content.width,
                pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                              metrics.homeMenuTopOffset + metrics.buttonHintsHeight)};
  const int menuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - static_cast<int>(recentBooks.size());
  compact_home::MenuPage menuPage{0, menuItemCount, menuSelection, menuItemCount};
#if FREEINK_DEVICE_XUESU_300
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer);
  const Rect body{safe.x, safe.y + metrics.topPadding, safe.width, safe.height - metrics.topPadding};
  const int headerHeight =
      std::max(metrics.batteryBarHeight, renderer.getLineHeight(SMALL_FONT_ID) + compact_home::GAP);
  const int iconMenuHeight =
      isCarousel ? metrics.menuRowHeight + renderer.getLineHeight(UI_10_FONT_ID) + compact_home::GAP : 0;
  const int rowHeight = compact_home::menuRowHeight(renderer.getLineHeight(UI_12_FONT_ID));
  const auto compact = compact_home::layout(body, headerHeight, iconMenuHeight, menuItemCount, rowHeight);
  headerRect = compact.header;
  coverRect = compact.cover;
  menuRect = compact.menu;
  if (!isCarousel) {
    menuPage = compact_home::menuPage(menuItemCount, menuSelection, menuRect.height, rowHeight);
  }
#endif
  auto drawHeader = [&] {
    GUI.drawHeader(renderer, headerRect,
                   metrics.homeShowRecentBookTitle && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);
  };
  auto drawMenu = [&] {
    auto menuEntry = [&](int index) -> HomeMenuEntry {
      const int menuIndex = menuPage.start + index;
      if (showContinueReading && menuIndex == 0) {
        return {HomeMenuItem::NONE, StrId::STR_CONTINUE_READING, Book};
      }
      auto entry = *menuEntryAtIndex(menuIndex - showContinueReading, hasLibrarySlot(), isCarousel);
      if (entry.item == HomeMenuItem::OPDS_BROWSER && hasPlugins) {
        entry.label = StrId::STR_PLUGINS;
        entry.icon = Plugins;
      }
      return entry;
    };
    GUI.drawHomeMenu(
        renderer, menuRect, menuPage.count, menuPage.selection,
        [&](int index) { return std::string(I18N.get(menuEntry(index).label)); },
        [&](int index) { return menuEntry(index).icon; });
#if FREEINK_DEVICE_XUESU_300
    if (!isCarousel) GUI.drawSideScrollBar(renderer, menuRect, menuItemCount, menuPage.start, menuPage.capacity);
#endif
  };

  const CarouselUpdateScope updateScope = carouselUpdateScope.exchange(CarouselUpdateScope::None);
  const bool menuOnlyUpdate = canRenderCarouselMenuOnly(isCarousel, recentsLoaded, updateScope);
  if (menuOnlyUpdate) {
    renderer.fillRect(headerRect.x, headerRect.y, headerRect.width, headerRect.height, false);
    drawHeader();
#if FREEINK_DEVICE_XUESU_300
    renderer.fillRect(menuRect.x, menuRect.y, menuRect.width, menuRect.height, false);
#else
    renderer.fillRect(0, pageHeight - metrics.menuRowHeight, pageWidth, metrics.menuRowHeight, false);
#endif
    drawMenu();
    renderer.displayBuffer();
    return;
  }

  if (isCarousel) {
    coverRendered = false;
    coverBufferStored = false;
  }

  renderer.clearScreen();
  if (coverGridUi) {
    coverGridUi->setSelection(selectorIndex);
    UITheme::getInstance().drawCoverGridHome(*coverGridUi);
    // Front Left/Right walk the tabs, so their hints read Left/Right; the
    // side page buttons (unhinted) walk the covers.
    const auto labels = mappedInput.mapLabels(backLabel, tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(!firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
#if FREEINK_DEVICE_XUESU_300
    firstRenderDone = true;
#else
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Refreshing the paths right away
    // lets the next pass draw already-cached thumbs before generation runs.
    const bool coverSpecChanged = coverGridUi->takeThumbHeightChanged();
    if (coverSpecChanged) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      requestUpdate();
    }
#endif
    return;
  }
#if FREEINK_DEVICE_XUESU_300
  // A rotation can change both the snapshot geometry and its thumbnail size.
  const int orientation = static_cast<int>(renderer.getOrientation());
  if (coverRectW > 0 && (coverRect.x != coverRectX || coverRect.y != coverRectY || coverRect.width != coverRectW ||
                         coverRect.height != coverRectH || orientation != coverOrientation)) {
    recentsLoaded = false;
    coverRendered = false;
    coverBufferStored = false;
  }
  coverOrientation = orientation;
#endif
  bool bufferRestored = coverRendered && coverBufferStored && restoreCoverBuffer();

  if (isCarousel) {
    drawHeader();
  } else {
    // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
    // homeTopPadding, so the height must shrink by topPadding or the band (and a
    // centered title, e.g. RoundedRaff's book title) sinks into the tile.
    // Home is the stack root: no back button in its header.
    const char* title = !FREEINK_DEVICE_XUESU_300 && metrics.homeContinueReadingInMenu && !recentBooks.empty()
                            ? recentBooks[0].title.c_str()
                            : nullptr;
    GUI.drawHeader(renderer, headerRect, title, nullptr, false);
  }

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = coverRect.x;
  coverRectY = coverRect.y;
  coverRectW = coverRect.width;
  coverRectH = coverRect.height;

  const auto drawCover = [&] {
    GUI.drawRecentBookCover(renderer, coverRect, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                            bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this));
  };
  if (FREEINK_DEVICE_XUESU_300) {
    const GfxRenderer::ClipScope clip(renderer, coverRect.x, coverRect.y, coverRect.width, coverRect.height);
    drawCover();
  } else {
    drawCover();
  }

  drawMenu();

  if (!isCarousel) {
    const auto labels = mappedInput.mapLabels(backLabel, tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer, coverRect);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
}

void HomeActivity::onRecentBookSelected(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onAppsOpen() { activityManager.goToApps(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(hasOpdsServers); }
