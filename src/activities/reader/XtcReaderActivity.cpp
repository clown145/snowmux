#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "AchievementsStore.h"
#include "CrossPointSettings.h"
#include "ProgressFile.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/AchievementPopupUtils.h"

bool XtcReaderActivity::loadBook() {
  auto loadedXtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
  if (!loadedXtc) {
    LOG_ERR("XTR", "Failed to allocate XTC object");
    return false;
  }
  if (!loadedXtc->load()) {
    LOG_ERR("XTR", "Failed to load XTC");
    return false;
  }
  xtc = std::move(loadedXtc);
  xtc->setupCacheDir();
  loadProgress();
  const uint32_t totalPages = xtc->getPageCount();
  const uint8_t startPercent =
      totalPages > 0 ? static_cast<uint8_t>(std::min<uint32_t>(100, (currentPage + 1) * 100 / totalPages)) : 0;
  READING_STATS.beginSession(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath(), startPercent,
                             "", 0);
  return true;
}

void XtcReaderActivity::onExit() {
  READING_STATS.endSession();
  ACHIEVEMENTS.recordSessionEnded(READING_STATS.getLastSessionSnapshot());
  showPendingAchievementPopups(renderer);
  ReaderActivity::onExit();
}

void XtcReaderActivity::openChapterSelection() {
  if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
    startActivityForResultWith<XtcReaderChapterSelectionActivity>(
        [this](const ActivityResult& result) {
          READING_STATS.resumeSession();
          if (!result.isCancelled) {
            currentPage = std::get<PageResult>(result.data).page;
            requestUpdate();
          }
        },
        xtc, currentPage);
  }
}

bool XtcReaderActivity::handleFormatInput() {
  READING_STATS.tickActiveSession();
  if (!xtc) {
    return false;
  }

  // Enter chapter selection activity on Confirm release or touch menu gesture
  const bool touchMenu = ReaderUtils::isTouchMenuGesture(renderer, mappedInput);
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || touchMenu) {
    if (touchMenu && xtc->hasChapters() && !xtc->getChapters().empty()) haptic_feedback::touchAction();
    openChapterSelection();
    return true;
  }
  return false;
}

bool XtcReaderActivity::handleZoneShortAction(const uint8_t action) {
  // XTC comics have no bookmark / dictionary / KOReader sync / percent jump:
  // only the base actions (orientation, frontlight, home) and the chapter jump
  // apply to short taps.
  if (action == CrossPointSettings::TAP_ZONE_CHAPTER || action == CrossPointSettings::TAP_ZONE_MENU) {
    openChapterSelection();
    return true;
  }
  return ReaderActivity::handleZoneShortAction(action);
}

bool XtcReaderActivity::handleZoneLongAction(const uint8_t action) {
  switch (action) {
    case CrossPointSettings::TAP_ZONE_LONG_CHAPTER:
    case CrossPointSettings::TAP_ZONE_LONG_MENU:
      // XTC has no reader menu: the chapter selector doubles as its menu.
      openChapterSelection();
      return true;
    default:
      return ReaderActivity::handleZoneLongAction(action);
  }
}

void XtcReaderActivity::applyInitialOrientation() { renderer.setOrientation(GfxRenderer::Orientation::Portrait); }

void XtcReaderActivity::renderBook() {
  if (!xtc) {
    return;
  }

  renderPage();
  saveProgress();
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const auto sb = SETTINGS.statusBarSpec();
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title = sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = xtc->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& chapter) {
    return currentPage >= chapter.startPage && currentPage <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(currentPage - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

Rect XtcReaderActivity::renderStatusBarOverlay() const {
  const auto sb = SETTINGS.statusBarSpec();
  const bool drawBottom = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM;
  const bool drawTop = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP;
  if (!drawBottom && !drawTop) {
    return Rect{};
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return Rect{};
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (drawBottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = drawBottom ? renderer.getScreenHeight() - orientedMarginBottom - clearY : statusBarHeight + 4;
  renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, std::move(pageInfo.title),
                    paddingBottom);
  // The theme can extend its progress bar into the oriented bottom margin.
  return Rect{0, clearY, renderer.getScreenWidth(), clearHeight + orientedMarginBottom};
}

void XtcReaderActivity::renderPage() {
  const auto showError = [this](const char* message) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, message, true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
  };
  xtc::PageInfo page{};
  if (!xtc->getPageInfo(currentPage, page) || page.width == 0 || page.height == 0) {
    LOG_ERR("XTR", "Failed to read dimensions for page %lu", currentPage);
    showError(tr(STR_PAGE_LOAD_ERROR));
    return;
  }

  const size_t colBytes = (page.height + 7) / 8;
  const size_t planeSize = page.width * colBytes;
  const size_t rowBytes = (page.width + 7) / 8;
  const size_t pageBufferSize = page.bitDepth == 2 ? planeSize * 2 : rowBytes * page.height;
  // One packed source page can exceed the task stack (30,000 B for 300x400 XTCH).
  // Release it after drawing so paused readers and menus do not retain it.
  auto pageBuffer = makeUniqueNoThrow<uint8_t[]>(pageBufferSize);
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%zu bytes)", pageBufferSize);
    showError(tr(STR_MEMORY_ERROR));
    return;
  }
  if (xtc->loadPage(currentPage, pageBuffer.get(), pageBufferSize) != pageBufferSize) {
    LOG_ERR("XTR", "Failed to load page %lu: bufferSize=%zu bitDepth=%u error=%s", currentPage, pageBufferSize,
            page.bitDepth, xtc::errorToString(xtc->getLastError()));
    showError(tr(STR_PAGE_LOAD_ERROR));
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int drawWidth = page.width;
  int drawHeight = page.height;
  int drawX = 0;
  int drawY = 0;
  if (drawWidth > screenWidth || drawHeight > screenHeight) {
    if (drawWidth * screenHeight > drawHeight * screenWidth) {
      drawHeight = std::max(1, drawHeight * screenWidth / drawWidth);
      drawWidth = screenWidth;
    } else {
      drawWidth = std::max(1, drawWidth * screenHeight / drawHeight);
      drawHeight = screenHeight;
    }
    drawX = (screenWidth - drawWidth) / 2;
    drawY = (screenHeight - drawHeight) / 2;
  }
  // Sample the source at each destination pixel's center. Both formats can
  // use the decoded page directly, with no extra scaled framebuffer.
  const auto sourceX = [&](int x) { return (x * page.width + page.width / 2) / drawWidth; };
  const auto sourceY = [&](int y) { return (y * page.height + page.height / 2) / drawHeight; };
  if (page.bitDepth == 2) {
    const uint8_t* lsb = pageBuffer.get() + planeSize;
    const auto grayLevel = [&](const int x, const int y) {
      const size_t byte = (page.width - 1 - x) * colBytes + y / 8;
      const int bit = 7 - y % 8;
      const uint8_t value = (((pageBuffer[byte] >> bit) & 1) << 1) | ((lsb[byte] >> bit) & 1);
      return xtc::xthGrayLevel(value);
    };
    const auto overlay = renderer.grayscaleCapabilities();
    const bool absolute =
        !overlay.supported() && renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
    const auto clearOutsideStatusBar = [&](const Rect& preserved) {
      if (preserved.height == 0) {
        renderer.clearScreen();
        return;
      }
      const int bottom = preserved.y + preserved.height;
      renderer.fillRect(0, 0, screenWidth, preserved.y, false);
      renderer.fillRect(0, bottom, screenWidth, screenHeight - bottom, false);
    };
    const auto drawBw = [&](const Rect& preserved) {
      clearOutsideStatusBar(preserved);
      for (int y = 0; y < drawHeight; ++y) {
        const int py = drawY + y;
        if (py >= preserved.y && py < preserved.y + preserved.height) continue;
        const int sy = sourceY(y);
        for (int x = 0; x < drawWidth; ++x) {
          const auto level = grayLevel(sourceX(x), sy);
          if (overlay.supported() ? level < 3 : GfxRenderer::ditherTwoBitPixel(level, drawX + x, drawY + y)) {
            renderer.drawPixel(drawX + x, drawY + y, true);
          }
        }
      }
    };
    drawBw(Rect{});
    const Rect statusBar = renderStatusBarOverlay();

    const bool clean = pagesUntilFullRefresh <= 1;
    const auto refresh = clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
    bool grayStarted = false;
    if (absolute) {
      grayStarted = renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute, refresh);
    } else if (overlay.supported()) {
      if (clean && overlay.base != HalDisplay::GrayscaleBase::Combined) {
        renderer.displayBuffer(refresh);
        renderer.preconditionGrayscale();
      } else {
        renderer.displayGrayscaleBase(refresh, DisplayRefreshContext::ContinuousReading);
      }
      grayStarted = true;
    }

    if (grayStarted) {
      for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
        // Absolute planes encode black and white just like the B/W frame. An
        // overlay uses zero bits in the bar to leave the base frame visible.
        if (absolute) {
          clearOutsideStatusBar(statusBar);
        } else {
          renderer.clearScreen(0x00);
        }
        for (int y = 0; y < drawHeight; ++y) {
          const int py = drawY + y;
          if (py >= statusBar.y && py < statusBar.y + statusBar.height) continue;
          const int sy = sourceY(y);
          for (int x = 0; x < drawWidth; ++x) {
            const auto pixel = GfxRenderer::mapTwoBitPixel(mode, grayLevel(sourceX(x), sy), absolute);
            if (pixel.draw) renderer.drawPixel(drawX + x, drawY + y, pixel.state);
          }
        }
        if (mode == GfxRenderer::GRAYSCALE_LSB) {
          renderer.copyGrayscaleLsbBuffers();
        } else {
          renderer.copyGrayscaleMsbBuffers();
        }
      }
      renderer.displayGrayBuffer();
      drawBw(absolute ? statusBar : Rect{});
      if (!absolute) renderStatusBarOverlay();
      renderer.cleanupGrayscaleWithFrameBuffer();
    }
    if (!grayStarted || !renderer.displayCommitted()) {
      renderer.displayBuffer(grayStarted ? HalDisplay::HALF_REFRESH : refresh);
    }
    if (renderer.displayCommitted()) {
      pagesUntilFullRefresh = clean ? SETTINGS.getRefreshFrequency() : pagesUntilFullRefresh - 1;
      markPageRendered();
    }
    return;
  }

  renderer.clearScreen();
  for (int y = 0; y < drawHeight; ++y) {
    const size_t row = sourceY(y) * rowBytes;
    for (int x = 0; x < drawWidth; ++x) {
      const int sx = sourceX(x);
      if (!(pageBuffer[row + sx / 8] & (0x80 >> (sx % 8)))) {
        renderer.drawPixel(drawX + x, drawY + y, true);
      }
    }
  }
  pageBuffer.reset();

  renderStatusBarOverlay();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  LOG_DBG("XTR", "Rendered page %lu/%lu (1-bit)", currentPage + 1, xtc->getPageCount());
  if (renderer.displayCommitted()) markPageRendered();
}

bool XtcReaderActivity::pageTurn(bool isForward) {
  if (!xtc) return false;
  READING_STATS.noteActivity();
  if (isForward) {
    if (currentPage < xtc->getPageCount()) {
      currentPage++;
      return true;
    }
  } else {
    if (currentPage > 0) {
      currentPage--;
      return true;
    }
  }
  return false;
}

bool XtcReaderActivity::skipPages(int amount) {
  if (!xtc) return false;
  READING_STATS.noteActivity();
  int newPage = static_cast<int>(currentPage) + amount;
  if (newPage < 0) newPage = 0;
  if (newPage > static_cast<int>(xtc->getPageCount())) newPage = static_cast<int>(xtc->getPageCount());
  if (newPage != static_cast<int>(currentPage)) {
    currentPage = static_cast<uint32_t>(newPage);
    return true;
  }
  return false;
}

bool XtcReaderActivity::isAtEndOfBook() const { return xtc && currentPage >= xtc->getPageCount(); }

void XtcReaderActivity::onReturnFromEndOfBook() {
  if (xtc && xtc->getPageCount() > 0) {
    currentPage = xtc->getPageCount() - 1;
  } else {
    currentPage = 0;
  }
}

void XtcReaderActivity::saveProgress() const {
  if (!xtc) return;
  const uint32_t totalPages = xtc->getPageCount();
  const uint8_t progressPercent =
      totalPages > 0 ? static_cast<uint8_t>(std::min<uint32_t>(100, (currentPage + 1) * 100 / totalPages)) : 0;
  READING_STATS.updateProgress(progressPercent, totalPages > 0 && currentPage + 1 >= totalPages, "", progressPercent);
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = (currentPage >> 16) & 0xFF;
  data[3] = (currentPage >> 24) & 0xFF;
  if (!ProgressFile::writeAtomic(xtc->getCachePath(), data, sizeof(data))) {
    LOG_ERR("XTC", "Failed to save progress: page %lu", currentPage);
  }
}

void XtcReaderActivity::loadProgress() {
  if (!xtc) return;
  HalFile f;
  if (Storage.openFileForRead("XTC", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      if (currentPage >= xtc->getPageCount() && xtc->getPageCount() > 0) {
        currentPage = xtc->getPageCount() - 1;
      }
      LOG_DBG("XTC", "Loaded progress: page %lu/%lu", currentPage + 1, xtc->getPageCount());
    }
  }
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    // XTC progress already counts the visible page as read.
    info.endProgressPercent = info.progressPercent;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
