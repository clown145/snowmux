#ifdef CROSSMUX_XUESU_300_TXT_TEST

#include <Arduino.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <Xuesu300StockLut.generated.h>
#include <esp_ota_ops.h>
#include <strings.h>

#include <array>
#include <cstdio>
#include <cstring>

#include "TxtEncoding.h"
#include "boards/Xuesu300Board.h"

namespace {

using namespace xuesu300;

constexpr int kWidth = panelHeight;
constexpr int kHeight = panelWidth;
constexpr int kStride = panelWidth / 8;
constexpr int kPlaneBytes = kStride * panelHeight;
constexpr int kMargin = 10;
constexpr int kFirstLine = 28;
constexpr int kLineHeight = 19;
constexpr int kBottom = kHeight - 24;
constexpr unsigned kMaxBooks = 64;
constexpr unsigned kPathBytes = 192;
constexpr unsigned kHistorySize = 128;
constexpr uint16_t kEnd = 0xffff;

u8g2_t graphics;
u8x8_display_info_t geometry;
std::array<uint8_t, kPlaneBytes> frame;
std::array<uint8_t, kPlaneBytes> previousFrame;

std::array<std::array<char, kPathBytes>, kMaxBooks> paths;
std::array<uint32_t, kHistorySize> history;
unsigned bookCount = 0;
unsigned selected = 0;
unsigned historySize = 0;
uint32_t pageNumber = 1;
uint32_t nextOffset = 0;
bool cardReady = false;
bool panelReady = false;
fs::File book;
txt_encoding::Encoding bookEncoding = txt_encoding::Encoding::Unknown;

bool waitPanel(uint32_t timeoutMs, bool busy) {
  const uint32_t start = millis();
  const int expected = busy == panelBusyActiveLow ? LOW : HIGH;
  while (digitalRead(panelPins.busy) != expected) {
    if (millis() - start >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

void resetPanel() {
  digitalWrite(panelPins.cs, HIGH);
  digitalWrite(panelPins.reset, HIGH);
  delay(10);
  digitalWrite(panelPins.reset, LOW);
  delay(panelResetPulseMs);
  digitalWrite(panelPins.reset, HIGH);
  delay(50);
}

void send(uint8_t command, const uint8_t* data = nullptr, size_t size = 0) {
  digitalWrite(panelPins.dc, LOW);
  digitalWrite(panelPins.cs, LOW);
  SPI.transfer(command);
  digitalWrite(panelPins.cs, HIGH);
  if (size == 0) return;
  digitalWrite(panelPins.dc, HIGH);
  digitalWrite(panelPins.cs, LOW);
  SPI.writeBytes(data, size);
  digitalWrite(panelPins.cs, HIGH);
}

bool present() {
  if (!panelReady) return false;

  resetPanel();
  if (!waitPanel(300, false)) {
    Serial.println("EPD reset timeout");
    panelReady = false;
    return false;
  }

  SPI.beginTransaction(SPISettings(4'000'000, MSBFIRST, SPI_MODE0));
  for (const auto& write : observedPanelInit) send(write.command, write.data, write.length);
  for (unsigned i = 0; i < 5; ++i) send(0x20 + i, stockFullLut[i], sizeof(stockFullLut[i]));
  send(0x10, previousFrame.data(), previousFrame.size());
  send(0x13, frame.data(), frame.size());
  const uint8_t border = 0xd7;
  const uint8_t automatic = 0xa5;
  send(0x50, &border, 1);
  send(0x17, &automatic, 1);
  SPI.endTransaction();

  if (!waitPanel(300, true) || !waitPanel(panelBusyTimeoutMs, false)) {
    Serial.println("EPD refresh timeout");
    resetPanel();
    panelReady = false;
    return false;
  }

  SPI.beginTransaction(SPISettings(4'000'000, MSBFIRST, SPI_MODE0));
  send(0x02);
  SPI.endTransaction();
  delay(2);
  if (!waitPanel(1000, false)) {
    Serial.println("EPD power-off timeout");
    resetPanel();
    panelReady = false;
    return false;
  }
  previousFrame = frame;
  Serial.println("EPD refresh OK");
  return true;
}

void drawLine(u8g2_t* context, u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t length, uint8_t direction) {
  for (unsigned i = 0; i < length; ++i) {
    const unsigned col = x + (direction == 0 ? i : 0);
    const unsigned row = y + (direction == 0 ? 0 : i);
    if (col >= kWidth || row >= kHeight) break;
    const unsigned nativeX = panelWidth - 1 - row;
    auto& byte = frame[col * kStride + nativeX / 8];
    const uint8_t mask = 0x80u >> (nativeX % 8);
    if (context->draw_color == 0)
      byte |= mask;
    else
      byte &= static_cast<uint8_t>(~mask);
  }
}

void setupDisplay() {
  for (const int pin : {panelPins.cs, panelPins.dc, panelPins.reset}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  pinMode(panelPins.busy, INPUT);
  SPI.begin(panelPins.sck, -1, panelPins.mosi, -1);

  geometry.pixel_width = kWidth;
  geometry.pixel_height = kHeight;
  geometry.tile_width = (kWidth + 7) / 8;
  geometry.tile_height = (kHeight + 7) / 8;
  u8g2_GetU8x8(&graphics)->display_info = &geometry;
  u8g2_SetupBuffer(&graphics, frame.data(), geometry.tile_height, drawLine, U8G2_R0);
  u8g2_SetFontMode(&graphics, 1);
  u8g2_SetDrawColor(&graphics, 1);
  previousFrame.fill(0xff);
  panelReady = true;
}

int prepareGlyph(uint16_t& codepoint) {
  u8g2_SetFont(&graphics, u8g2_font_wqy14_t_gb2312);
  if (codepoint >= 32) {
    if (const int width = u8g2_GetGlyphWidth(&graphics, codepoint); width > 0) return width;
    // The Japanese subset covers kanji outside GB2312, such as U+702C.
    u8g2_SetFont(&graphics, u8g2_font_unifont_t_japanese3);
    if (const int width = u8g2_GetGlyphWidth(&graphics, codepoint); width > 0) return width;
    u8g2_SetFont(&graphics, u8g2_font_wqy14_t_gb2312);
  }
  codepoint = '?';
  return u8g2_GetGlyphWidth(&graphics, codepoint);
}

void printAt(int x, int y, const char* text) {
  auto* decoder = u8g2_GetU8x8(&graphics);
  u8x8_utf8_init(decoder);
  for (; *text != '\0' && x < kWidth; ++text) {
    uint16_t codepoint = u8x8_utf8_next(decoder, static_cast<uint8_t>(*text));
    if (codepoint == 0xffff) break;
    if (codepoint == 0xfffe) continue;
    const int width = prepareGlyph(codepoint);
    u8g2_DrawGlyph(&graphics, x, y, codepoint);
    x += width;
  }
}

const char* baseName(const char* path) {
  const auto* slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}

void scanDirectory(const char* path, unsigned depth) {
  auto dir = SD_MMC.open(path, FILE_READ);
  if (!dir || !dir.isDirectory()) return;
  for (auto item = dir.openNextFile(); item && bookCount < kMaxBooks; item = dir.openNextFile()) {
    const char* itemPath = item.path();
    if (!itemPath || baseName(itemPath)[0] == '.') continue;
    if (item.isDirectory()) {
      if (depth > 0) scanDirectory(itemPath, depth - 1);
      continue;
    }
    const auto length = std::strlen(itemPath);
    if (length >= 4 && length < kPathBytes && strcasecmp(itemPath + length - 4, ".txt") == 0) {
      std::memcpy(paths[bookCount++].data(), itemPath, length + 1);
    }
  }
}

void loadShelf() {
  if (!cardReady) {
    pinMode(sdEnablePin, OUTPUT);
    digitalWrite(sdEnablePin, HIGH);
    delay(50);
    cardReady = SD_MMC.setPins(sdPins.clk, sdPins.cmd, sdPins.d0, sdPins.d1, sdPins.d2, sdPins.d3) &&
                SD_MMC.begin("/sdcard", false, false, 20000, 8);
  }
  if (!cardReady) {
    Serial.println("SD mount failed");
    return;
  }
  Serial.printf("SD mounted, %llu MiB\n", static_cast<unsigned long long>(SD_MMC.cardSize() / (1024 * 1024)));
  bookCount = 0;
  scanDirectory("/", 3);
  Serial.printf("TXT files: %u\n", bookCount);
}

void drawShelf() {
  frame.fill(0xff);
  printAt(kMargin, 20, cardReady ? "TXT" : "SD!");
  if (bookCount == 0 && cardReady) printAt(kMargin, 52, "TXT?");
  constexpr unsigned kRows = 17;
  const unsigned start = selected / kRows * kRows;
  for (unsigned i = start; i < bookCount && i < start + kRows; ++i) {
    const int y = 46 + (i - start) * kLineHeight;
    if (i == selected) printAt(kMargin, y, ">");
    printAt(kMargin + 17, y, baseName(paths[i].data()));
  }
  present();
}

uint16_t readCodepoint() {
  const int first = book.read();
  if (first < 0) return kEnd;
  if (first < 0x80) return first;

  if (bookEncoding == txt_encoding::Encoding::Gbk) {
    const int second = book.peek();
    if (second < 0) return '?';
    const uint16_t cp = txt_encoding::decodeGbkPair(first, second);
    if (cp == 0) return '?';
    book.read();
    return cp;
  }

  if (first < 0xc2 || first > 0xf4) return '?';
  const unsigned length = first < 0xe0 ? 2 : (first < 0xf0 ? 3 : 4);
  uint32_t cp = first & ((1u << (7 - length)) - 1);
  for (unsigned i = 1; i < length; ++i) {
    const int byte = book.peek();
    if (byte < 0x80 || byte >= 0xc0) return '?';
    cp = (cp << 6) | (book.read() & 0x3f);
  }
  if ((length == 3 && cp < 0x800) || (cp >= 0xd800 && cp <= 0xdfff) || cp >= 0xfffe || length == 4) return '?';
  return cp;
}

const char* encodingName() { return bookEncoding == txt_encoding::Encoding::Gbk ? "GBK" : "UTF-8"; }

void detectBookEncoding() {
  std::array<uint8_t, 2048> sample;
  const size_t count = book.read(sample.data(), sample.size());
  bookEncoding = txt_encoding::detect(sample.data(), count, count < sample.size());
  Serial.printf("TXT encoding: %s, size: %lu bytes\n", encodingName(), static_cast<unsigned long>(book.size()));
}

bool drawPage(uint32_t offset) {
  if (!book.seek(offset)) {
    Serial.printf("TXT seek failed at %lu\n", static_cast<unsigned long>(offset));
    return false;
  }
  frame.fill(0xff);
  int x = kMargin;
  int y = kFirstLine;
  while (y < kBottom) {
    const uint32_t pos = book.position();
    uint16_t cp = readCodepoint();
    if (cp == kEnd) break;
    if (cp == '\r' || cp == 0xfeff) continue;
    if (cp == '\n') {
      x = kMargin;
      y += kLineHeight;
      continue;
    }
    if (cp == '\t') cp = ' ';
    const int width = prepareGlyph(cp);
    if (x + width > kWidth - kMargin) {
      x = kMargin;
      y += kLineHeight;
      if (y >= kBottom) {
        if (!book.seek(pos)) return false;
        break;
      }
    }
    u8g2_DrawGlyph(&graphics, x, y, cp);
    x += width;
  }
  char pageLabel[24];
  snprintf(pageLabel, sizeof(pageLabel), "%lu", static_cast<unsigned long>(pageNumber));
  printAt(kMargin, kHeight - 8, pageLabel);
  if (!present()) return false;
  nextOffset = book.position();
  Serial.printf("TXT page %lu, bytes %lu..%lu\n", static_cast<unsigned long>(pageNumber),
                static_cast<unsigned long>(offset), static_cast<unsigned long>(nextOffset));
  return true;
}

void onKey(uint8_t key) {
  if (!book) {
    if (key == 4) {
      if (bookCount == 0) {
        loadShelf();
        drawShelf();
        return;
      }
      book = SD_MMC.open(paths[selected].data(), FILE_READ);
      if (!book) {
        Serial.println("TXT open failed");
        return;
      }
      detectBookEncoding();
      historySize = 1;
      history[0] = 0;
      pageNumber = 1;
      if (!drawPage(0)) book.close();
      return;
    }
    if (bookCount < 2) return;
    if (key == 1)
      selected = selected == 0 ? bookCount - 1 : selected - 1;
    else if (key == 2)
      selected = selected + 1 == bookCount ? 0 : selected + 1;
    else
      return;
    drawShelf();
    return;
  }

  if (key == 4) {
    book.close();
    drawShelf();
  } else if (key == 1 && historySize > 1) {
    --pageNumber;
    if (drawPage(history[historySize - 2]))
      --historySize;
    else
      ++pageNumber;
  } else if (key == 2 && nextOffset > history[historySize - 1] && nextOffset < book.size()) {
    const uint32_t offset = nextOffset;
    ++pageNumber;
    if (!drawPage(offset)) {
      --pageNumber;
      return;
    }
    if (historySize == kHistorySize) {
      std::memmove(history.data(), history.data() + 1, (kHistorySize - 1) * sizeof(uint32_t));
      --historySize;
    }
    history[historySize++] = offset;
  }
}

uint8_t keyMask() {
  return (digitalRead(previousKeyPin) == HIGH ? 1 : 0) | (digitalRead(nextKeyPin) == HIGH ? 2 : 0) |
         (digitalRead(confirmKeyPin) == LOW ? 4 : 0);
}

}  // namespace

// This app does not use NVS. Prevent Arduino startup from initializing or
// repairing the stock firmware's settings partition.
extern "C" esp_err_t __wrap_nvs_flash_init() { return ESP_OK; }

void setup() {
  Serial.begin(115200);
  pinMode(previousKeyPin, INPUT);
  pinMode(nextKeyPin, INPUT);
  pinMode(confirmKeyPin, INPUT_PULLUP);
  setupDisplay();
  loadShelf();
  drawShelf();
}

void loop() {
  static uint8_t raw = keyMask();
  static uint8_t stable = raw;
  static uint8_t gesture = 0;
  static uint32_t changedAt = 0;
  const uint8_t sample = keyMask();
  if (sample != raw) {
    raw = sample;
    changedAt = millis();
  }
  if (stable != raw && millis() - changedAt >= 30) {
    const uint8_t previous = stable;
    stable = raw;
    if (stable == 0) {
      onKey(gesture);
      gesture = 0;
    } else if (previous == 0)
      gesture = stable;
    else
      gesture = 0;
  }
  if (Serial.available() && Serial.read() == '?') {
    const auto* running = esp_ota_get_running_partition();
    Serial.printf("Xuesu TXT test, slot=%s, SD=%s, TXT=%u, EPD=%s, encoding=%s, page=%lu, next=%lu\n",
                  running ? running->label : "?", cardReady ? "OK" : "FAIL", bookCount, panelReady ? "OK" : "FAIL",
                  book ? encodingName() : "-", static_cast<unsigned long>(book ? pageNumber : 0),
                  static_cast<unsigned long>(book ? nextOffset : 0));
  }
  delay(5);
}

#endif
