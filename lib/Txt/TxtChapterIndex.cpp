#include "TxtChapterIndex.h"

#include <cctype>

namespace txt_chapter_index {
namespace {

constexpr std::string_view FULLWIDTH_SPACE = "　";

bool isAsciiSpace(const char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

bool startsWithSpace(const std::string_view text) {
  return !text.empty() && (isAsciiSpace(text.front()) || text.starts_with(FULLWIDTH_SPACE));
}

void skipSpaces(std::string_view& text) {
  while (startsWithSpace(text)) text.remove_prefix(isAsciiSpace(text.front()) ? 1 : FULLWIDTH_SPACE.size());
}

std::string_view trim(std::string_view text) {
  skipSpaces(text);
  while (!text.empty()) {
    if (isAsciiSpace(text.back())) {
      text.remove_suffix(1);
    } else if (text.ends_with(FULLWIDTH_SPACE)) {
      text.remove_suffix(FULLWIDTH_SPACE.size());
    } else {
      break;
    }
  }
  return text;
}

bool consume(std::string_view& text, const std::string_view token) {
  if (!text.starts_with(token)) return false;
  text.remove_prefix(token.size());
  return true;
}

int consumeDigit(std::string_view& text) {
  if (text.empty()) return -1;
  if (text.front() >= '0' && text.front() <= '9') {
    const int digit = text.front() - '0';
    text.remove_prefix(1);
    return digit;
  }
  if (text.size() >= 3 && text.starts_with("\xEF\xBC")) {
    const auto last = static_cast<unsigned char>(text[2]);
    if (last >= 0x90 && last <= 0x99) {
      text.remove_prefix(3);
      return last - 0x90;
    }
  }
  return -1;
}

bool consumeChineseNumeral(std::string_view& text) {
  static constexpr std::string_view NUMERALS[] = {"零", "〇", "一", "二", "三", "四", "五", "六", "七", "八", "九",
                                                  "十", "百", "千", "万", "萬", "亿", "億", "两", "兩", "廿"};
  for (const auto numeral : NUMERALS) {
    if (consume(text, numeral)) return true;
  }
  return false;
}

bool consumeChineseNumber(std::string_view& text) {
  const size_t originalSize = text.size();
  while (consumeDigit(text) >= 0 || consumeChineseNumeral(text)) {
  }
  return text.size() < originalSize;
}

struct Bracket {
  std::string_view open;
  std::string_view close;
};

constexpr Bracket BRACKETS[] = {{"【", "】"}, {"〖", "〗"}, {"〔", "〕"}, {"［", "］"}, {"（", "）"},
                                {"「", "」"}, {"『", "』"}, {"《", "》"}, {"[", "]"},   {"(", ")"}};

std::string_view headingText(const std::string_view title) {
  for (const auto& [open, close] : BRACKETS) {
    if (title.starts_with(open)) {
      const size_t end = title.find(close, open.size());
      if (end != std::string_view::npos) return trim(title.substr(open.size(), end - open.size()));
      break;
    }
  }
  return title;
}

bool isChineseHeading(std::string_view text) {
  if (!consume(text, "第")) return false;
  skipSpaces(text);
  if (!consumeChineseNumber(text)) return false;
  skipSpaces(text);

  static constexpr std::string_view PROSE_PREFIXES[] = {"节课", "節課", "回合", "部分", "集合"};
  for (const auto prefix : PROSE_PREFIXES) {
    if (text.starts_with(prefix)) return false;
  }
  static constexpr std::string_view UNITS[] = {"章", "节", "節", "回", "卷", "部", "篇", "集", "话", "話", "册", "冊"};
  for (const auto unit : UNITS) {
    if (text.starts_with(unit)) return true;
  }
  return false;
}

bool endsTitleWord(const std::string_view text) {
  if (text.empty() || startsWithSpace(text)) return true;
  static constexpr std::string_view SEPARATORS[] = {":", "：", "-", "－", "—", "、", "(", "（"};
  for (const auto separator : SEPARATORS) {
    if (text.starts_with(separator)) return true;
  }
  return false;
}

bool isSpecialHeading(std::string_view text) {
  if (consume(text, "番外")) {
    if (!consume(text, "篇")) consumeChineseNumber(text);
    return endsTitleWord(text);
  }
  static constexpr std::string_view TITLES[] = {"序章", "楔子", "引子", "前言", "终章", "終章",
                                                "尾声", "尾聲", "后记", "後記", "附录", "附錄"};
  for (const auto title : TITLES) {
    if (text.starts_with(title) && endsTitleWord(text.substr(title.size()))) return true;
  }
  return false;
}

bool equalsAsciiCaseInsensitive(const std::string_view text, const std::string_view expected) {
  if (text.size() < expected.size()) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) != expected[i]) return false;
  }
  return true;
}

bool isEnglishHeading(std::string_view text) {
  static constexpr std::string_view PREFIXES[] = {"chapter", "book", "part", "volume"};
  for (const auto prefix : PREFIXES) {
    if (!equalsAsciiCaseInsensitive(text, prefix)) continue;
    text.remove_prefix(prefix.size());
    if (!startsWithSpace(text)) return false;
    skipSpaces(text);

    const size_t originalSize = text.size();
    while (consumeDigit(text) >= 0) {
    }
    if (text.size() == originalSize) {
      while (!text.empty() && std::string_view("ivxlcdm").find(
                                  std::tolower(static_cast<unsigned char>(text.front()))) != std::string_view::npos) {
        text.remove_prefix(1);
      }
    }
    return text.size() < originalSize && (endsTitleWord(text) || text.starts_with('.') || text.starts_with("．"));
  }
  return false;
}

bool readHeadingNumber(std::string_view text, uint32_t& number) {
  constexpr size_t MAX_DIGITS = 5;
  size_t digits = 0;
  for (int digit = consumeDigit(text); digit >= 0; digit = consumeDigit(text)) {
    if (++digits > MAX_DIGITS) return false;
    number = number * 10 + static_cast<uint32_t>(digit);
  }
  if (!digits) return false;
  if (text.empty() || startsWithSpace(text)) return true;

  static constexpr std::string_view SEPARATORS[] = {".", "．", ":", "：", "、", "-", "－", "—"};
  for (const auto separator : SEPARATORS) {
    if (consume(text, separator)) {
      // Reject decimals, timestamps and numeric date components.
      return !text.empty() && consumeDigit(text) < 0;
    }
  }
  for (const auto& [open, close] : BRACKETS) {
    if (text.starts_with(open)) return text.ends_with(close) && text.size() > open.size() + close.size();
  }
  return false;
}

}  // namespace

Heading matchHeading(std::string_view line) {
  line = trim(line);
  if (consume(line, "\xEF\xBB\xBF")) line = trim(line);
  if (line.empty()) return {HeadingKind::Empty};
  if (line.size() >= TITLE_CAPACITY || line.find('\0') != std::string_view::npos) return {HeadingKind::Body};
  const auto text = headingText(line);
  if (isChineseHeading(text) || isSpecialHeading(text) || isEnglishHeading(text)) {
    return {HeadingKind::Standalone, line};
  }
  uint32_t number = 0;
  if (readHeadingNumber(text, number)) return {HeadingKind::Numbered, line, number};
  return {HeadingKind::Body};
}

}  // namespace txt_chapter_index
