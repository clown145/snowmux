#if FREEINK_DEVICE_XUESU_300

#include "XuesuLegacyData.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>

#include "RecentBooksStore.h"

namespace {
constexpr char kLegacyRecents[] = "/.ereader/recent_books.json";
constexpr size_t kMaxRecentsBytes = 32 * 1024;

bool loadRecents(JsonDocument& doc) {
  if (!Storage.exists(kLegacyRecents)) return false;
  std::string json;
  if (!Storage.readFileToString("LEGACY", kLegacyRecents, kMaxRecentsBytes, json)) return false;
  JsonDocument filter;
  filter["version"] = true;
  filter["items"][0]["path"] = true;
  filter["items"][0]["title"] = true;
  filter["items"][0]["progress_percent"] = true;
  if (deserializeJson(doc, json, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(8)) ||
      doc["version"].as<int>() != 1 || !doc["items"].is<JsonArray>()) {
    LOG_ERR("LEGACY", "Invalid or unsupported stock recents");
    return false;
  }
  return true;
}

std::string bookPath(JsonObjectConst item) {
  const auto raw = item["path"].as<std::string_view>();
  if (raw.empty() || raw.size() > 1024 || raw.find('\\') != std::string_view::npos) return {};
  auto path = FsHelpers::normalisePath(std::string(raw));
  if (path.empty()) return {};
  if (path.front() != '/') path.insert(path.begin(), '/');
  if (path.starts_with("/.") || !FsHelpers::hasReflowableBookExtension(path)) return {};
  return path;
}
}  // namespace

std::optional<float> xuesu_legacy::recentProgress(const std::string_view path) {
  JsonDocument doc;
  if (!loadRecents(doc)) return std::nullopt;
  for (const auto item : doc["items"].as<JsonArrayConst>()) {
    const auto percent = item["progress_percent"];
    if (!percent.is<float>() || bookPath(item.as<JsonObjectConst>()) != path) continue;
    const float value = percent.as<float>();
    if (std::isfinite(value) && value > 0.0f && value <= 100.0f) return value / 100.0f;
  }
  return std::nullopt;
}

int xuesu_legacy::importRecentBooks() {
  JsonDocument doc;
  if (!loadRecents(doc)) return -1;
  const auto& existing = RECENT_BOOKS.getBooks();
  if (existing.size() >= RecentBooksStore::MAX_RECENT_BOOKS) return 0;
  // Up to ten entries on this cold path. Move their strings into the recents store on success.
  std::vector<RecentBook> imported;
  imported.reserve(RecentBooksStore::MAX_RECENT_BOOKS - existing.size());
  for (const auto item : doc["items"].as<JsonArrayConst>()) {
    if (existing.size() + imported.size() >= RecentBooksStore::MAX_RECENT_BOOKS) break;
    auto path = bookPath(item.as<JsonObjectConst>());
    const auto matches = [&path](const RecentBook& book) { return book.path == path; };
    if (path.empty() || std::any_of(existing.begin(), existing.end(), matches) ||
        std::any_of(imported.begin(), imported.end(), matches))
      continue;
    auto file = Storage.open(path.c_str());
    if (!file || file.isDirectory()) continue;
    auto title = item["title"].as<std::string_view>();
    if (title.size() > 1024) title = {};
    imported.push_back({std::move(path), std::string(title), {}, {}});
  }
  const int count = imported.size();
  return count == 0 || RECENT_BOOKS.appendImportedBooks(std::move(imported)) ? count : -1;
}

#endif
