#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#if FREEINK_DEVICE_XUESU_300
#include <ArduinoJson.h>
#include <HalClock.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#endif
// clang-format on

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "CrossMuxEndpoints.h"
#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"

namespace {
constexpr std::string_view nightlyTagPrefix = "nightly-";
constexpr size_t nightlyShaLength = 7;

constexpr bool isSameNightlyBuild(const std::string_view currentVersion, const std::string_view latestTag) {
  if (!latestTag.starts_with(nightlyTagPrefix) || latestTag.size() != nightlyTagPrefix.size() + nightlyShaLength) {
    return false;
  }

  const std::string_view latestSha = latestTag.substr(nightlyTagPrefix.size());
  const bool validSha = std::all_of(latestSha.begin(), latestSha.end(), [](const char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
  if (!validSha) return false;

  const size_t separator = currentVersion.rfind('+');
  return separator != std::string_view::npos && currentVersion.substr(separator + 1) == latestSha;
}

static_assert(isSameNightlyBuild("1.5.2-rc+5064d90", "nightly-5064d90"));
static_assert(isSameNightlyBuild("1.5.2-cn-rc+5064d90", "nightly-5064d90"));
static_assert(!isSameNightlyBuild("1.5.2-rc+5064d90", "nightly-1234567"));
static_assert(!isSameNightlyBuild("1.5.2", "nightly-5064d90"));
static_assert(!isSameNightlyBuild("1.5.2-rc+5064d90", "nightly"));

// The language-selected content profile supplies the OTA host and release asset variant.
// The web proxy re-exposes it as a minimal
// GitHub-release-shaped JSON whose single asset is always named "firmware.bin"
// — that's the literal ReleaseJsonParser matches on.
//
// Going through the web instead of api.github.com directly avoids the
// unauthenticated 60 req/hr/IP rate limit and the unstable mainland-China
// path to api.github.com.
constexpr size_t releaseUrlCapacity = 192;
static_assert(releaseUrlCapacity < 256);

#if FREEINK_DEVICE_XUESU_300
constexpr char xuesuManifestUrl[] = "https://clown145.github.io/snowmux/api/v1/firmware/latest.json";
constexpr std::string_view xuesuImageBase = "https://clown145.github.io/snowmux/firmware/";
constexpr char xuesuMirrorFormat[] =
    "https://gh-proxy.com/https://github.com/clown145/snowmux/releases/download/xuesu-v%s/xuesu-v%s-ota1.bin";
constexpr int xuesuMirrorTimeoutMs = 10'000;
constexpr int xuesuOtaTimeoutMs = 15'000;
constexpr size_t xuesuSlotSize = 0x730000;
constexpr size_t xuesuManifestLimit = 4096;
using ImageHash = std::array<uint8_t, 32>;
using ReleaseVersion = std::array<unsigned, 3>;

constexpr char xuesuFactoryOrigin[] = "https://xiaomoreader.top";
constexpr char xuesuFactoryManifest[] = "https://xiaomoreader.top/XueBingOTA/version.json";
constexpr char xuesuFactoryModel[] = "xuesu-300-std";

bool parseReleaseVersion(std::string_view text, ReleaseVersion& version) {
  for (size_t i = 0; i < version.size(); ++i) {
    const char* end = text.data() + text.size();
    const auto [next, error] = std::from_chars(text.data(), end, version[i]);
    if (error != std::errc{}) return false;
    if (i == version.size() - 1) return next == end;
    if (next == end || *next != '.') return false;
    text.remove_prefix(next - text.data() + 1);
  }
  return false;
}

bool readImageHash(std::string_view url, ImageHash& hash) {
  constexpr std::string_view suffix = ".bin";
  if (!url.starts_with(xuesuImageBase) || !url.ends_with(suffix) ||
      url.size() != xuesuImageBase.size() + hash.size() * 2 + suffix.size()) {
    return false;
  }
  url.remove_prefix(xuesuImageBase.size());
  for (auto& byte : hash) {
    const auto [next, error] = std::from_chars(url.data(), url.data() + 2, byte, 16);
    if (error != std::errc{} || next != url.data() + 2) return false;
    url.remove_prefix(2);
  }
  return true;
}

bool isXuesuAppSlot(const esp_partition_t* partition) {
  if (!partition || partition->type != ESP_PARTITION_TYPE_APP || partition->size != xuesuSlotSize) return false;
  return (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 && partition->address == 0x20000) ||
         (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 && partition->address == 0x750000);
}
#endif
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate(const Channel requestedChannel,
                                                       const AbortCallback& shouldAbort) {
#if FREEINK_DEVICE_PICCO
  return UNSUPPORTED_CHANNEL;
#endif
  updateAvailable = false;
  if (shouldAbort && shouldAbort()) return CANCELLED;
  channel = requestedChannel;
#if FREEINK_DEVICE_XUESU_300
  if (channel == Channel::Nightly) return UNSUPPORTED_CHANNEL;
  if (!halClock.hasValidTime()) return HTTP_ERROR;
  if (channel == Channel::Factory) return checkFactoryRelease(shouldAbort);
  const char* releaseUrl = xuesuManifestUrl;
#else
  char releaseUrl[releaseUrlCapacity];
  const char* channelQuery = channel == Channel::Nightly ? "&channel=nightly" : "";
  const int releaseUrlLength =
      snprintf(releaseUrl, sizeof(releaseUrl), CrossMuxEndpoints::OTA_MANIFEST_FORMAT, CrossMuxEndpoints::host(),
               CrossMuxEndpoints::otaVariant(), channelQuery, HalSystem::getDeviceModel());
  if (releaseUrlLength < 0 || static_cast<size_t>(releaseUrlLength) >= sizeof(releaseUrl)) {
    LOG_ERR("OTA", "Release URL exceeds %zu bytes", sizeof(releaseUrl));
    return INTERNAL_UPDATE_ERROR;
  }
#endif
  LOG_DBG("OTA", "Checking %s channel (current: %s)", channel == Channel::Nightly ? "nightly" : "stable",
          CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the configured secure GET transport,
  // redirects, and User-Agent (see HttpDownloader).
  // The shared SAX token is 2 KiB; keep it off the network task stack.
  auto releaseParser = makeUniqueNoThrow<ReleaseJsonParser>(releaseNotes);
  if (!releaseParser) {
    LOG_ERR("OTA", "OOM: release parser (%zu bytes)", sizeof(ReleaseJsonParser));
    return INTERNAL_UPDATE_ERROR;
  }
#if FREEINK_DEVICE_XUESU_300
  size_t manifestBytes = 0;
#endif
  const auto feedRelease = [&](const uint8_t* data, size_t len) {
#if FREEINK_DEVICE_XUESU_300
    if (len > xuesuManifestLimit - manifestBytes) return false;
    manifestBytes += len;
#endif
    releaseParser->feed(reinterpret_cast<const char*>(data), len);
    return true;
  };
#if FREEINK_DEVICE_XUESU_300
  const bool ok = HttpDownloader::fetchVerifiedHttps(releaseUrl, feedRelease, xuesuOtaTimeoutMs, shouldAbort);
#else
  const bool ok = HttpDownloader::fetchUrl(releaseUrl, feedRelease, "", "", {}, shouldAbort);
#endif
  if (shouldAbort && shouldAbort()) return CANCELLED;
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser->foundTag() ? "yes" : "no",
          releaseParser->foundFirmware() ? "yes" : "no");

  if (releaseParser->foundUnsupportedChannel()) {
    LOG_INF("OTA", "Selected update channel is not supported by this device");
    return UNSUPPORTED_CHANNEL;
  }

  if (!releaseParser->foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser->foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

#if FREEINK_DEVICE_XUESU_300
  ReleaseVersion version{};
  ImageHash hash{};
  const size_t imageSize = releaseParser->getFirmwareSize();
  if (!parseReleaseVersion(releaseParser->getTagName(), version) ||
      !readImageHash(releaseParser->getFirmwareUrl(), hash) || imageSize < 64 || imageSize > xuesuSlotSize ||
      imageSize % 16 != 0) {
    LOG_ERR("OTA", "Invalid Xuesu release metadata");
    return JSON_PARSE_ERROR;
  }
#endif
  latestVersion = releaseParser->getTagName();
  otaUrl = releaseParser->getFirmwareUrl();
  otaSize = releaseParser->getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;
  releaseNoteCount = releaseParser->getReleaseNoteCount();

  LOG_DBG("OTA", "Found update: tag=%s size=%zu notes=%zu", latestVersion.c_str(), otaSize, releaseNoteCount);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

#if FREEINK_DEVICE_XUESU_300
OtaUpdater::OtaUpdaterError OtaUpdater::checkFactoryRelease(const AbortCallback& shouldAbort) {
  // The vendor publishes a multi-device JSON document. Read it into one
  // bounded heap buffer so the update activity does not need a larger stack.
  constexpr size_t manifestCapacity = 8192;
  const auto manifest = makeUniqueNoThrow<char[]>(manifestCapacity);
  if (!manifest) {
    LOG_ERR("OTA", "OOM allocating %zu bytes for factory manifest", manifestCapacity);
    return OOM_ERROR;
  }
  size_t length = 0;
  const bool ok = HttpDownloader::fetchVerifiedHttps(
      xuesuFactoryManifest,
      [&](const uint8_t* data, size_t len) {
        if (len > manifestCapacity - length) return false;
        std::memcpy(manifest.get() + length, data, len);
        length += len;
        return true;
      },
      xuesuOtaTimeoutMs, shouldAbort);
  if (shouldAbort && shouldAbort()) return CANCELLED;
  if (!ok) return HTTP_ERROR;

  JsonDocument filter;
  JsonObject fields = filter["devices"][xuesuFactoryModel].to<JsonObject>();
  fields["version_name"] = true;
  fields["version_code"] = true;
  fields["url"] = true;
  fields["size"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, manifest.get(), length, DeserializationOption::Filter(filter))) return JSON_PARSE_ERROR;
  const JsonVariantConst release = doc["devices"][xuesuFactoryModel].as<JsonVariantConst>();
  const std::string_view version = release["version_name"] | "";
  const std::string_view url = release["url"] | "";
  const size_t size = release["size"] | 0u;
  const bool relativePath = url.starts_with('/') && !url.starts_with("//");
  if (version.empty() || version.size() >= 32 || (release["version_code"] | 0u) == 0 || size < 64 ||
      size > xuesuSlotSize || url.size() > 511 || (!relativePath && !url.starts_with("https://"))) {
    LOG_ERR("OTA", "Invalid official Xuesu release metadata");
    return JSON_PARSE_ERROR;
  }

  latestVersion.assign(version);
  otaUrl = relativePath ? xuesuFactoryOrigin : "";
  otaUrl.append(url);
  otaSize = totalSize = size;
  releaseNoteCount = 0;
  updateAvailable = true;
  return OK;
}
#endif

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }
#if FREEINK_DEVICE_XUESU_300
  if (channel == Channel::Factory) return true;
  constexpr std::string_view current = CROSSPOINT_VERSION;
  ReleaseVersion installed{}, latest{};
  return parseReleaseVersion(current.substr(0, current.find('-')), installed) &&
         parseReleaseVersion(latestVersion, latest) && latest > installed;
#else
  switch (channel) {
    case Channel::Stable:
      break;
    case Channel::Nightly:
      return !isSameNightlyBuild(CROSSPOINT_VERSION, latestVersion);
  }
  if (latestVersion == CROSSPOINT_VERSION) return false;

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
#endif
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx,
                                                      const AbortCallback& shouldAbort) {
#if FREEINK_DEVICE_PICCO
  return UNSUPPORTED_CHANNEL;
#endif
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

#if FREEINK_DEVICE_XUESU_300
  if (channel == Channel::Stable) {
    char mirrorUrl[releaseUrlCapacity];
    const int length =
        snprintf(mirrorUrl, sizeof(mirrorUrl), xuesuMirrorFormat, latestVersion.c_str(), latestVersion.c_str());
    if (length > 0 && static_cast<size_t>(length) < sizeof(mirrorUrl)) {
      const auto result = installUpdateFromSource(mirrorUrl, onProgress, ctx, shouldAbort);
      if (result == OK) {
        LOG_INF("OTA", "Xuesu update verified from gh-proxy.com");
        return OK;
      }
      if (result != HTTP_ERROR && result != VERIFICATION_ERROR && result != WRONG_DEVICE_ERROR) return result;
      if (WiFi.status() != WL_CONNECTED) return HTTP_ERROR;

      // The mirror attempt has been aborted. Start a fresh write from the canonical Pages URL.
      LOG_INF("OTA", "Xuesu mirror failed (%d), retrying from Pages", static_cast<int>(result));
      processedSize = 0;
      if (onProgress) onProgress(ctx, InstallPhase::Downloading);
    }
  }
#endif
  return installUpdateFromSource(nullptr, onProgress, ctx, shouldAbort);
}

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdateFromSource([[maybe_unused]] const char* mirrorUrl,
                                                                ProgressCallback onProgress, void* ctx,
                                                                const AbortCallback& shouldAbort) {
  if (shouldAbort && shouldAbort()) return CANCELLED;
  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }

#if FREEINK_DEVICE_XUESU_300
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!isXuesuAppSlot(running) || !isXuesuAppSlot(updatePartition) || running->address == updatePartition->address ||
      esp_ota_get_boot_partition() != running || otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Xuesu OTA slots or boot selection do not match the running firmware");
    return INTERNAL_UPDATE_ERROR;
  }
  const bool restoringFactory = channel == Channel::Factory;
  ImageHash expectedHash{};
  if (!restoringFactory && !readImageHash(otaUrl, expectedHash)) return VERIFICATION_ERROR;
  constexpr size_t imageWriteSize = OTA_WITH_SEQUENTIAL_WRITES;
#else
  constexpr auto imageWriteSize = OTA_SIZE_UNKNOWN;
#endif
  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, imageWriteSize, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  // The image streams in chunks; only the first bytes carry the header. Buffer
  // the first 14 bytes so we can read chip_id (esp_image_header_t offset 12)
  // and reject a wrong-MCU image before it overwrites the OTA partition.
  uint8_t hdr[14];
  size_t hdrLen = 0;
  bool wrongChip = false;
  board_tag::Scanner boardScanner;
  bool wrongBoard = false;
#if FREEINK_DEVICE_XUESU_300
  mbedtls_sha256_context hashContext;
  bool verificationOk = true;
  if (!restoringFactory) {
    mbedtls_sha256_init(&hashContext);
    verificationOk = mbedtls_sha256_starts(&hashContext, 0) == 0;
  }
#endif
  const auto writeChunk = [&](const uint8_t* data, size_t len) {
#if FREEINK_DEVICE_XUESU_300
    if (!verificationOk || len > otaSize - processedSize ||
        (!restoringFactory && mbedtls_sha256_update(&hashContext, data, len) != 0)) {
      verificationOk = false;
      return false;
    }
#endif
    if (hdrLen < sizeof(hdr)) {
      const size_t take = std::min(len, sizeof(hdr) - hdrLen);
      std::memcpy(hdr + hdrLen, data, take);
      hdrLen += take;
      if (hdrLen == sizeof(hdr)) {
        uint16_t imageChip;
        std::memcpy(&imageChip, hdr + 12, sizeof(imageChip));
#if FREEINK_DEVICE_XUESU_300
        constexpr uint16_t deviceChip = 9;  // ESP32-S3 image chip ID.
#else
        const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
#endif
        if (deviceChip != 0xFFFF && imageChip != deviceChip) {
          LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
          wrongChip = true;
          return false;  // abort the transfer
        }
      }
    }
#if FREEINK_DEVICE_XUESU_300
    if (!restoringFactory) boardScanner.feed(data, len);
#else
    boardScanner.feed(data, len);
#endif
    if (boardScanner.mismatch()) {
      LOG_ERR("OTA", "wrong board: image=%s device=%.*s", boardScanner.foundName(),
              static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
      wrongBoard = true;
      return false;  // abort before selecting the incomplete image as bootable
    }
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx, InstallPhase::Downloading);
      }
    }
    return true;
  };
#if FREEINK_DEVICE_XUESU_300
  const bool fetchOk = verificationOk && HttpDownloader::fetchVerifiedHttps(
                                             mirrorUrl ? mirrorUrl : otaUrl.c_str(), writeChunk,
                                             mirrorUrl ? xuesuMirrorTimeoutMs : xuesuOtaTimeoutMs, shouldAbort);
  ImageHash actualHash{};
  if (fetchOk) {
    verificationOk =
        processedSize == otaSize &&
        (restoringFactory || (boardScanner.matched() && mbedtls_sha256_finish(&hashContext, actualHash.data()) == 0 &&
                              actualHash == expectedHash));
  }
  if (!restoringFactory) mbedtls_sha256_free(&hashContext);
#else
  const bool fetchOk = HttpDownloader::fetchUrl(otaUrl, writeChunk, "", "", {}, shouldAbort);
#endif

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (shouldAbort && shouldAbort()) {
    esp_ota_abort(otaHandle);
    return CANCELLED;
  }
#if FREEINK_DEVICE_XUESU_300
  if (!verificationOk) {
    LOG_ERR("OTA", "Xuesu image failed size, board tag or SHA-256 verification");
    esp_ota_abort(otaHandle);
    return VERIFICATION_ERROR;
  }
#endif
  if (wrongChip || wrongBoard) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  if (onProgress) onProgress(ctx, InstallPhase::Finalizing);
  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}

#if FREEINK_DEVICE_XUESU_300
bool OtaUpdater::targetsOta0() const {
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  return dest && dest->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0;
}
#endif
