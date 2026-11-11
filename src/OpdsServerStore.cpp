#include "OpdsServerStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>

namespace {
void encodeServer(JsonObject obj, const OpdsServer& server) {
  obj["name"] = server.name;
  obj["url"] = server.url;
  obj["username"] = server.username;
  obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
  JsonArray headers = obj["headers"].to<JsonArray>();
  for (const auto& header : server.customHeaders) {
    JsonObject headerObj = headers.add<JsonObject>();
    headerObj["name"] = header.name;
    headerObj["value_obf"] = obfuscation::obfuscateToBase64(header.value);
  }
}
}  // namespace

void OpdsServerStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : servers) encodeServer(arr.add<JsonObject>(), server);
}

bool OpdsServerStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'servers' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  servers.clear();
  JsonArrayConst arr = doc["servers"].as<JsonArrayConst>();
  servers.reserve(std::min(arr.size(), MAX_SERVERS));
  bool needsResave = false;

  for (JsonObjectConst obj : arr) {
    if (servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | "";
    server.url = obj["url"] | "";
    server.username = obj["username"] | "";
    server.password = extractPassword(obj, needsResave);

    size_t headerIndex = 0;
    for (JsonObjectConst headerObj : obj["headers"].as<JsonArrayConst>()) {
      if (headerIndex >= OpdsServer::MAX_CUSTOM_HEADERS) break;
      server.customHeaders[headerIndex].name = headerObj["name"] | "";
      server.customHeaders[headerIndex].value = obfuscation::deobfuscateFromBase64(headerObj["value_obf"] | "");
      if ((!server.customHeaders[headerIndex].name.empty() || !server.customHeaders[headerIndex].value.empty()) &&
          !isValidHttpHeader(server.customHeaders[headerIndex])) {
        server.customHeaders[headerIndex] = {};
        needsResave = true;
      }
      headerIndex++;
    }

    servers.push_back(std::move(server));
  }

  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", servers.size());

  if (needsResave) {
    LOG_DBG("OPS", "Resaving JSON with obfuscated passwords");
    requestResave();
  }

  return true;
}

bool OpdsServerStore::addServer(const OpdsServer& server) {
  std::lock_guard<std::mutex> lock(storeMutex);
  for (const auto& header : server.customHeaders) {
    if (!header.name.empty() && !isValidHttpHeader(header)) {
      LOG_ERR("OPS", "Invalid custom HTTP header");
      return false;
    }
  }
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return false;
  }

  // Reserve the fixed eight-record limit before committing; reuse the existing
  // serialization document instead of cloning every server for rollback.
  servers.reserve(MAX_SERVERS);
  JsonDocument doc;
  toJson(doc);
  encodeServer(doc["servers"].as<JsonArray>().add<JsonObject>(), server);
  if (!writeDocToFile(getFilePath(), doc, true)) return false;
  servers.push_back(server);
  LOG_DBG("OPS", "Added server: %s", server.name.c_str());
  return true;
}

bool OpdsServerStore::updateServer(size_t index, const OpdsServer& server) {
  std::lock_guard<std::mutex> lock(storeMutex);
  for (const auto& header : server.customHeaders) {
    if (!header.name.empty() && !isValidHttpHeader(header)) {
      LOG_ERR("OPS", "Invalid custom HTTP header");
      return false;
    }
  }
  if (index >= servers.size()) {
    return false;
  }

  JsonDocument doc;
  toJson(doc);
  encodeServer(doc["servers"][index].to<JsonObject>(), server);
  if (!writeDocToFile(getFilePath(), doc, true)) return false;
  servers[index] = server;
  LOG_DBG("OPS", "Updated server: %s", server.name.c_str());
  return true;
}

bool OpdsServerStore::removeServer(size_t index) {
  std::lock_guard<std::mutex> lock(storeMutex);
  if (index >= servers.size()) {
    return false;
  }

  JsonDocument doc;
  toJson(doc);
  doc["servers"].as<JsonArray>().remove(index);
  if (!writeDocToFile(getFilePath(), doc, true)) return false;
  LOG_DBG("OPS", "Removed server: %s", servers[index].name.c_str());
  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  return true;
}

const OpdsServer* OpdsServerStore::getServer(size_t index) const {
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}

std::vector<HttpDownloader::Header> OpdsServer::activeCustomHeaders() const {
  std::vector<HttpDownloader::Header> active;
  active.reserve(MAX_CUSTOM_HEADERS);
  for (const auto& header : customHeaders) {
    if (isValidHttpHeader(header)) active.emplace_back(header.name, header.value);
  }
  return active;
}
