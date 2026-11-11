#include "WifiProvisionActivity.h"

#if FREEINK_DEVICE_XUESU_300

#include <ArduinoJson.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstdio>
#include <string_view>

#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/html/PhoneSetupPageHtml.generated.h"
#include "network/html/WifiSetupPageHtml.generated.h"
#include "network/html/assets/crosspoint_iconSvg.generated.h"
#include "network/html/assets/webCss.generated.h"
#include "util/QrUtils.h"

namespace {
constexpr unsigned long kConnectionTimeout = 15000;
constexpr unsigned long kSessionTimeout = 5UL * 60 * 1000;
constexpr unsigned long kSuccessDisplayTime = 8000;
constexpr int kMaxScanNetworks = 24;
constexpr int kQrQuietZone = 4;
constexpr int kMinimumQrSide = (33 + 2 * kQrQuietZone) * 3;  // Version 4, at least 3 px per module.
constexpr int kMaximumQrSide = (33 + 2 * kQrQuietZone) * 5;

bool isPortalRead(const HTTPMethod method) { return method == HTTP_GET || method == HTTP_HEAD; }

bool validCredential(const std::string_view ssid, const std::string_view password) {
  if (ssid.empty() || ssid.size() > 32 || ssid.find('\0') != ssid.npos || password.find('\0') != password.npos) {
    return false;
  }
  if (password.size() == 64) {
    return password.find_first_not_of("0123456789abcdefABCDEF") == password.npos;
  }
  return password.empty() || (password.size() >= 8 && password.size() <= 63);
}

void sendJson(WebServer& web, const int status, const JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  web.send(status, "application/json", body);
}

void sendGzip(WebServer& web, const char* contentType, const char* data, const size_t size) {
  web.sendHeader("Content-Encoding", "gzip");
  web.send_P(200, contentType, data, size);
}
}  // namespace

void WifiProvisionActivity::onEnter() {
  Activity::onEnter();
  sessionStart = millis();
  previousMode = WiFi.getMode();
  if (!form) WIFI_STORE.loadFromFile();
  const auto mode = form && !WiFi.STA.connected() ? WIFI_AP : WIFI_AP_STA;
  if (!NetworkStartup::setMode(renderer, mode)) {
    setState(State::Unavailable);
    return;
  }
  ownsRadio = true;
  if (!form) {
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(false);
    WiFi.disconnect(false, false, 1000);
  }
  const IPAddress portalIp(192, 168, 4, 1);
  if (!WiFi.softAPConfig(portalIp, portalIp, IPAddress(255, 255, 255, 0), IPAddress{}, portalIp)) {
    LOG_ERR("WSET", "Could not advertise portal DNS over DHCP");
  }
  snprintf(hotspot, sizeof(hotspot), "CrossMux-Setup-%04lX", static_cast<unsigned long>(esp_random() & 0xffff));
  snprintf(hotspotPassword, sizeof(hotspotPassword), "%08lx%04lx", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random() & 0xffff));
  if (!WiFi.softAP(hotspot, hotspotPassword, 1, false, 1)) {
    setState(State::Unavailable);
    return;
  }
  const auto ip = WiFi.softAPIP();
  apAddress = ip.toString();
  web = makeUniqueNoThrow<WebServer>(ip, 80);
  if (!web) {
    LOG_ERR("WSET", "Could not allocate setup WebServer");
    setState(State::Unavailable);
    return;
  }
  // A connectivity probe has an external Host. Redirect reads to the literal
  // AP address, while keeping credential submissions on the local origin.
  web->addMiddleware([this](WebServer& request, Middleware::Callback next) {
    request.sendHeader("Cache-Control", "no-store");
    const auto host = request.hostHeader();
    if (host != apAddress && host != apAddress + ":80") {
      if (isPortalRead(request.method())) {
        redirectToPortal(request);
      } else {
        request.send(403);
      }
      return true;
    }
    return next();
  });
  const char* headers[] = {"Content-Type"};
  web->collectHeaders(headers, std::size(headers));
  web->on("/", HTTP_GET, [this] {
    if (form)
      sendGzip(*web, "text/html; charset=utf-8", PhoneSetupPageHtml, PhoneSetupPageHtmlCompressedSize);
    else
      sendGzip(*web, "text/html; charset=utf-8", WifiSetupPageHtml, WifiSetupPageHtmlCompressedSize);
  });
  web->on("/", HTTP_HEAD, [this] { web->send(200, "text/html; charset=utf-8", ""); });
  web->on("/assets/web.css", HTTP_GET,
          [this] { sendGzip(*web, "text/css; charset=utf-8", webCss, webCssCompressedSize); });
  web->on("/assets/crosspoint-icon.svg", HTTP_GET,
          [this] { sendGzip(*web, "image/svg+xml", crosspoint_iconSvg, crosspoint_iconSvgCompressedSize); });
  if (form) {
    web->on("/form", HTTP_GET, [this] { sendForm(); });
    web->on("/form", HTTP_POST, [this] { submitForm(); });
  } else {
    web->on("/networks", HTTP_GET, [this] { sendNetworks(); });
    web->on("/scan", HTTP_POST, [this] { startScan(); });
    web->on("/connect", HTTP_POST, [this] { startConnection(); });
  }
  web->on("/status", HTTP_GET, [this] { sendStatus(); });
  web->onNotFound([this] {
    if (isPortalRead(web->method())) {
      redirectToPortal(*web);
    } else {
      web->send(404);
    }
  });
  web->begin();
  startPortalDns();
  if (!form) {
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false, false, 120);
  }
  setState(State::Ready);
  LOG_INF("WSET", "Setup hotspot ready");
}

void WifiProvisionActivity::onExit() {
  portalDns.stop();
  web.reset();
  if (ownsRadio) {
    if (form) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(previousMode);
    } else if (handoff) {
      esp_wifi_scan_stop();
      WiFi.scanDelete();
      WiFi.softAPdisconnect(true);
      WiFi.setAutoReconnect(true);
    } else {
      esp_wifi_scan_stop();
      WiFi.scanDelete();
      WiFi.mode(WIFI_OFF);
    }
  }
  form.reset();
  Activity::onExit();
}

void WifiProvisionActivity::sendForm() {
  JsonDocument doc;
  form->describe(doc);
  doc["saved"] = state == State::Saved;
  sendJson(*web, 200, doc);
}

void WifiProvisionActivity::submitForm() {
  if (state == State::Saved) {
    web->send(200);  // A retried request cannot create another server.
    return;
  }
  if (web->header("Content-Type") != "application/json") {
    web->send(415);
    return;
  }
  const String body = web->arg("plain");
  JsonDocument doc;
  if (body.length() > 8192 || deserializeJson(doc, body)) {
    web->send(400);
    return;
  }
  switch (form->accept(doc)) {
    case PhoneSetupForm::Result::Invalid:
      web->send(400);
      return;
    case PhoneSetupForm::Result::SaveFailed:
      setState(State::SaveFailed);
      web->send(500);
      return;
    case PhoneSetupForm::Result::Saved:
      joinedAt = millis();
      setState(State::Saved);
      web->send(200);
      return;
  }
}

void WifiProvisionActivity::setState(const State next) {
  state.store(next);
  requestUpdate();
}

void WifiProvisionActivity::startPortalDns() {
  // The Arduino DNS listener binds all interfaces. Run it only while the
  // station is disconnected, and keep answers out of the phone's DNS cache.
  if (WiFi.STA.connected()) return;
  portalDns.setTTL(0);
  if (!portalDns.start(53, "*", WiFi.softAPIP())) {
    LOG_ERR("WSET", "Could not start portal DNS");
  }
}

void WifiProvisionActivity::redirectToPortal(WebServer& request) const {
  request.sendHeader("Location", "http://" + apAddress + "/", true);
  request.send(302, "text/plain", "");
}

void WifiProvisionActivity::startScan() {
  if (web->header("Content-Type") != "application/json") {
    web->send(415);
    return;
  }
  if (state == State::Connecting || state == State::Connected) {
    web->send(409);
    return;
  }
  if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false, false, 120);
  }
  web->send(202);
}

void WifiProvisionActivity::sendNetworks() {
  const int count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) {
    web->send(202);
    return;
  }
  JsonDocument doc;
  auto networks = doc.to<JsonArray>();
  for (int i = 0; i < std::min(count, kMaxScanNetworks); ++i) {
    const String name = WiFi.SSID(i);
    if (name.isEmpty()) continue;
    auto item = networks.add<JsonObject>();
    item["ssid"] = name;
    item["rssi"] = WiFi.RSSI(i);
    item["secured"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  sendJson(*web, 200, doc);
}

void WifiProvisionActivity::startConnection() {
  if (web->header("Content-Type") != "application/json") {
    web->send(415);
    return;
  }
  if (state == State::Connecting || state == State::Connected) {
    web->send(409);
    return;
  }
  const String body = web->arg("plain");
  JsonDocument doc;
  if (body.length() > 512 || deserializeJson(doc, body)) {
    web->send(400);
    return;
  }
  const auto name = doc["ssid"].as<JsonString>();
  const auto key = doc["password"].as<JsonString>();
  if (name.isNull() || key.isNull() || !validCredential({name.c_str(), name.size()}, {key.c_str(), key.size()})) {
    web->send(400);
    return;
  }
  ssid.assign(name.c_str(), name.size());
  password.assign(key.c_str(), key.size());
  web->send(202);

  esp_wifi_scan_stop();
  WiFi.scanDelete();
  WiFi.disconnect(false);
  portalDns.stop();
  attemptStart = millis();
  setState(State::Connecting);
  WiFi.begin(ssid.c_str(), password.empty() ? nullptr : password.c_str());
}

void WifiProvisionActivity::checkConnection() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress{} && WiFi.SSID() == ssid.c_str()) {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      if (WiFi.disconnect(false, false, 1000)) startPortalDns();
      password.clear();
      setState(State::SaveFailed);
      return;
    }
    WIFI_STORE.setLastConnectedSsid(ssid);
    password.clear();
    stationAddress = WiFi.localIP().toString().c_str();
    joinedAt = millis();
    setState(State::Connected);
    LOG_INF("WSET", "Station acquired IP and credentials saved");
  } else if (millis() - attemptStart >= kConnectionTimeout) {
    if (WiFi.disconnect(false, false, 1000)) startPortalDns();
    password.clear();
    setState(State::Failed);
    LOG_INF("WSET", "Station connection timed out");
  }
}

void WifiProvisionActivity::sendStatus() {
  JsonDocument doc;
  switch (state.load()) {
    case State::Connecting:
      doc["state"] = "connecting";
      break;
    case State::Connected:
      doc["state"] = "connected";
      doc["ip"] = stationAddress;
      break;
    case State::Saved:
      doc["state"] = "saved";
      break;
    case State::Failed:
      doc["state"] = "failed";
      break;
    case State::SaveFailed:
      doc["state"] = "save-failed";
      break;
    default:
      doc["state"] = "ready";
      break;
  }
  sendJson(*web, 200, doc);
}

void WifiProvisionActivity::complete(const bool connected) {
  ActivityResult result;
  result.isCancelled = !connected;
  handoff = !form && connected && keepStation;
  if (!form && connected) result.data = WifiResult{true, ssid, stationAddress};
  setResult(std::move(result));
  finish();
}

void WifiProvisionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    complete(state == State::Connected || state == State::Saved);
    return;
  }
  if (web) web->handleClient();
  const auto phase = state.load();
  using Button = MappedInputManager::Button;
  if (phase != State::Connected && phase != State::Saved && phase != State::Unavailable &&
      (mappedInput.wasReleased(Button::NavPrevious) || mappedInput.wasReleased(Button::NavNext))) {
    showPassword.store(!showPassword.load());
    requestUpdate();
  }
  if (phase == State::Connecting) {
    checkConnection();
  } else if (phase == State::Connected || phase == State::Saved) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || millis() - joinedAt >= kSuccessDisplayTime) {
      complete(true);
    }
  } else if (millis() - sessionStart >= kSessionTimeout) {
    complete(false);
  }
}

void WifiProvisionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const auto safe = theme.getScreenSafeArea(renderer, true);
  GUI.drawHeader(renderer, SubpageLayout::headerRect(safe, metrics),
                 form ? tr(STR_PHONE_SETUP) : tr(STR_WIFI_HOTSPOT_SETUP));
  const auto content =
      SubpageLayout::insetHorizontal(SubpageLayout::contentRect(safe, metrics), metrics.contentSidePadding);
  const auto phase = state.load();
  if (phase == State::Unavailable || phase == State::Starting) {
    UITheme::drawCenteredWrappedText(renderer, content, UI_10_FONT_ID,
                                     phase == State::Unavailable ? tr(STR_ERROR_GENERAL_FAILURE) : tr(STR_LOADING), 3);
  } else {
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int rowHeight = lineHeight + 5;
    const char* status = nullptr;
    if (phase == State::Connecting) status = tr(STR_CONNECTING);
    if (phase == State::Failed) status = tr(STR_CONNECTION_FAILED);
    if (phase == State::SaveFailed) status = tr(STR_FAILED_LOWER);
    const int statusRows = status ? 1 : 0;
    const int qrSide = std::min({content.width, content.height - (5 + statusRows) * rowHeight, kMaximumQrSide});
    const bool canShowQr = qrSide >= kMinimumQrSide;
    const bool showQr = canShowQr && !showPassword.load();
    const bool done = phase == State::Connected || phase == State::Saved;
    const int rows = done ? 2 : (showQr ? 5 : (canShowQr ? 7 : 6)) + statusRows;
    const int bodyHeight = rows * rowHeight + (!done && showQr ? qrSide : 0);
    int y = SubpageLayout::centeredTop(content, bodyHeight);
    const auto line = [&](const char* text, const bool bold = false) {
      const Rect row{content.x, y, content.width, lineHeight};
      UITheme::drawCenteredWrappedText(renderer, row, UI_10_FONT_ID, text, 1, true,
                                       bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      y += rowHeight;
    };
    if (phase == State::Saved) {
      line(tr(STR_PHONE_SETUP_SAVED), true);
      line(tr(STR_PHONE_SETUP_RETURN));
    } else if (phase == State::Connected) {
      line(tr(STR_CONNECTED), true);
      line(stationAddress.c_str());
    } else {
      line(showQr ? tr(STR_WIFI_SETUP_SCAN_HOTSPOT) : tr(STR_WIFI_SETUP_JOIN_HOTSPOT));
      if (showQr) {
        // The generated SSID and password contain only alphanumerics and hyphens.
        // Neither field needs escaping in the Wi-Fi QR payload.
        const std::string payload = std::string("WIFI:T:WPA;S:") + hotspot + ";P:" + hotspotPassword + ";;";
        QrUtils::drawQrCode(renderer, Rect{content.x + (content.width - qrSide) / 2, y, qrSide, qrSide}, payload,
                            kQrQuietZone);
        y += qrSide;
      }
      line(hotspot, true);
      if (!showQr) {
        line(tr(STR_WIFI_SETUP_HOTSPOT_PASSWORD));
        line(hotspotPassword, true);
      }
      line(tr(STR_WIFI_SETUP_OPEN_BROWSER));
      const String url = "http://" + apAddress;
      line(url.c_str(), true);
      if (canShowQr) line(showQr ? tr(STR_WIFI_SETUP_SHOW_PASSWORD) : tr(STR_WIFI_SETUP_SHOW_QR));
      if (status) line(status);
    }
  }
  renderer.displayBuffer();
}

#endif
