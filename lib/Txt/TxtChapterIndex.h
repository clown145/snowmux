#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace txt_chapter_index {

constexpr size_t TITLE_CAPACITY = 192;

struct Record {
  uint32_t sourceOffset = 0;
  char title[TITLE_CAPACITY] = {};
};
static_assert(sizeof(Record) == 196);

enum class HeadingKind : uint8_t { Empty, Body, Standalone, Numbered };

struct Heading {
  HeadingKind kind;
  std::string_view title = {};
  uint32_t number = 0;
};

// The input is UTF-8. Numbered candidates need a sequence before they can enter the index.
Heading matchHeading(std::string_view line);

}  // namespace txt_chapter_index
